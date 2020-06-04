// Copyright 2018 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "DolphinQt/FIFO/FIFOAnalyzer.h"

#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSplitter>
#include <QTextBrowser>
#include <QTreeWidget>
#include <QTreeWidgetItem>

#include "Common/Assert.h"
#include "Common/MathUtil.h"
#include "Common/Swap.h"
#include "Core/FifoPlayer/FifoAnalyzer.h"
#include "Core/FifoPlayer/FifoPlayer.h"

#include "DolphinQt/Settings.h"

#include "VideoCommon/BPMemory.h"
#include "VideoCommon/OpcodeDecoding.h"
#include "VideoCommon/VideoCommon.h"
#include "VideoCommon/XFMemory.h"

constexpr int FRAME_ROLE = Qt::UserRole;
constexpr int OBJECT_ROLE = Qt::UserRole + 1;
constexpr int LAYER_ROLE = Qt::UserRole + 2;
constexpr int EFBCOPY_ROLE = Qt::UserRole + 3;
constexpr int TYPE_ROLE = Qt::UserRole + 4;
constexpr int VERTEX_ROLE = Qt::UserRole + 5;
constexpr int VSIZE_ROLE0 = Qt::UserRole + 6;
constexpr int VSIZE_ROLE7 = VSIZE_ROLE0 + 7;

constexpr int TYPE_WHOLE = 1;
constexpr int TYPE_FRAME = 2;
constexpr int TYPE_XFBCOPY = 3;
constexpr int TYPE_INHERITED_LAYER = 4;
constexpr int TYPE_LAYER = 5;
constexpr int TYPE_EFBCOPY = 6;
constexpr int TYPE_OBJECT = 7;

constexpr int LINE_LENGTH = 100;

const char* primitive_names[] = {"Quads",        "Quads_2", "Triangles",  "Triangle Strip",
                                 "Triangle Fan", "Lines",   "Line Strip", "Points"};

FIFOAnalyzer::FIFOAnalyzer()
{
  CreateWidgets();
  ConnectWidgets();

  UpdateTree();

  auto& settings = Settings::GetQSettings();

  m_object_splitter->restoreState(
      settings.value(QStringLiteral("fifoanalyzer/objectsplitter")).toByteArray());
  m_search_splitter->restoreState(
      settings.value(QStringLiteral("fifoanalyzer/searchsplitter")).toByteArray());

  m_detail_list->setFont(Settings::Instance().GetDebugFont());
  m_entry_detail_browser->setFont(Settings::Instance().GetDebugFont());

  connect(&Settings::Instance(), &Settings::DebugFontChanged, [this] {
    m_detail_list->setFont(Settings::Instance().GetDebugFont());
    m_entry_detail_browser->setFont(Settings::Instance().GetDebugFont());
  });
}

FIFOAnalyzer::~FIFOAnalyzer()
{
  auto& settings = Settings::GetQSettings();

  settings.setValue(QStringLiteral("fifoanalyzer/objectsplitter"), m_object_splitter->saveState());
  settings.setValue(QStringLiteral("fifoanalyzer/searchsplitter"), m_search_splitter->saveState());
}

void FIFOAnalyzer::CreateWidgets()
{
  m_tree_widget = new QTreeWidget;
  m_detail_list = new QListWidget;
  m_entry_detail_browser = new QTextBrowser;

  m_object_splitter = new QSplitter(Qt::Horizontal);

  m_object_splitter->addWidget(m_tree_widget);
  m_object_splitter->addWidget(m_detail_list);

  m_tree_widget->header()->hide();
  m_tree_widget->setSelectionMode(QAbstractItemView::SelectionMode::ContiguousSelection);

  m_search_box = new QGroupBox(tr("Search Current Object"));
  m_search_edit = new QLineEdit;
  m_search_new = new QPushButton(tr("Search"));
  m_search_next = new QPushButton(tr("Next Match"));
  m_search_previous = new QPushButton(tr("Previous Match"));
  m_search_label = new QLabel;

  auto* box_layout = new QHBoxLayout;

  box_layout->addWidget(m_search_edit);
  box_layout->addWidget(m_search_new);
  box_layout->addWidget(m_search_next);
  box_layout->addWidget(m_search_previous);
  box_layout->addWidget(m_search_label);

  m_search_box->setLayout(box_layout);

  m_search_box->setMaximumHeight(m_search_box->minimumSizeHint().height());

  m_search_splitter = new QSplitter(Qt::Vertical);

  m_search_splitter->addWidget(m_object_splitter);
  m_search_splitter->addWidget(m_entry_detail_browser);
  m_search_splitter->addWidget(m_search_box);

  auto* layout = new QHBoxLayout;
  layout->addWidget(m_search_splitter);

  setLayout(layout);
}

void FIFOAnalyzer::ConnectWidgets()
{
  connect(m_tree_widget, &QTreeWidget::itemSelectionChanged, this, &FIFOAnalyzer::UpdateDetails);
  connect(m_detail_list, &QListWidget::itemSelectionChanged, this,
          &FIFOAnalyzer::UpdateDescription);

  connect(m_search_new, &QPushButton::clicked, this, &FIFOAnalyzer::BeginSearch);
  connect(m_search_next, &QPushButton::clicked, this, &FIFOAnalyzer::FindNext);
  connect(m_search_previous, &QPushButton::clicked, this, &FIFOAnalyzer::FindPrevious);
}

void FIFOAnalyzer::Update()
{
  UpdateTree();
  UpdateDetails();
  UpdateDescription();
}

static bool AlmostEqual(float a, float b)
{
  constexpr const float epsilon = 0.001f;
  return fabs(a - b) < epsilon;
}

static bool AlmostEqual(MathUtil::Rectangle<float> r, MathUtil::Rectangle<float> r2)
{
  return AlmostEqual(r.left, r2.left) && AlmostEqual(r.right, r2.right) &&
         AlmostEqual(r.top, r2.top) && AlmostEqual(r.bottom, r2.bottom);
}

static bool AlmostEqual(MathUtil::Rectangle<float> r, MathUtil::Rectangle<float> r2,
                        MathUtil::Rectangle<float> r3)
{
  return AlmostEqual(r, r2) && AlmostEqual(r2, r3);
}

QString FIFOAnalyzer::DescribeLayer(bool set_viewport, bool set_scissor, bool set_projection)
{
  QString result;
  const int xoff = m_bpmem->scissorOffset.x * 2;
  const int yoff = m_bpmem->scissorOffset.y * 2;
  MathUtil::Rectangle<float> r_scissor(m_bpmem->scissorTL.x - xoff, m_bpmem->scissorTL.y - yoff,
                                       m_bpmem->scissorBR.x - xoff + 1,
                                       m_bpmem->scissorBR.y - yoff + 1);
  if (!set_scissor)
  {
    r_scissor.left = -1;
    r_scissor.right = -1;
    r_scissor.top = -1;
    r_scissor.bottom = -1;
  }

  float x = m_xfmem->viewport.xOrig - m_xfmem->viewport.wd - xoff;
  float y = m_xfmem->viewport.yOrig + m_xfmem->viewport.ht - yoff;
  float width = 2.0f * m_xfmem->viewport.wd;
  float height = -2.0f * m_xfmem->viewport.ht;
  if (width < 0.f)
  {
    x += width;
    width *= -1;
  }
  if (height < 0.f)
  {
    y += height;
    height *= -1;
  }
  MathUtil::Rectangle<float> r_viewport(x, y, x + width, y + height);
  float min_depth = (m_xfmem->viewport.farZ - m_xfmem->viewport.zRange) / 16777216.0f;
  float max_depth = m_xfmem->viewport.farZ / 16777216.0f;
  if (!set_viewport)
  {
    r_viewport.left = -2;
    r_viewport.right = -2;
    r_viewport.top = -2;
    r_viewport.bottom = -2;
    min_depth = 0;
    max_depth = 1;
  }

  float near_z = -3;
  float far_z = -3;
  if (set_projection && m_xfmem->projection.type == GX_ORTHOGRAPHIC)
  {
    width = 2 / m_xfmem->projection.rawProjection[0];
    height = -2 / m_xfmem->projection.rawProjection[2];
    x = (-m_xfmem->projection.rawProjection[1] - 1) * width / 2;
    y = (m_xfmem->projection.rawProjection[3] - 1) * height / 2;
    float a = m_xfmem->projection.rawProjection[4];
    float b = m_xfmem->projection.rawProjection[5];
    near_z = (b + 1) / a;
    far_z = b / a;
  }
  else
  {
    x = -3;
    y = -3;
    width = -3;
    height = -3;
  }
  MathUtil::Rectangle<float> r_projection(x, y, x + width, y + height);

  // Viewport
  if (AlmostEqual(r_viewport, r_scissor, r_projection))
  {
    result = QStringLiteral("VP+Scissor+Proj 2D");
    if (r_viewport.left != 0 || r_viewport.top != 0)
      result += QStringLiteral(" (%1, %2)").arg(r_viewport.left).arg(r_viewport.top);
    result += QStringLiteral(" %1x%2, near %3 far %4")
                  .arg(r_viewport.GetWidth())
                  .arg(r_viewport.GetHeight())
                  .arg(near_z)
                  .arg(far_z);
    set_projection = false;
    set_scissor = false;
  }
  else if (AlmostEqual(r_viewport, r_scissor))
  {
    result = QStringLiteral("VP+Scissor");
    if (r_viewport.left != 0 || r_viewport.top != 0)
      result += QStringLiteral(" (%1, %2)").arg(r_viewport.left).arg(r_viewport.top);
    result += QStringLiteral(" %1x%2").arg(r_viewport.GetWidth()).arg(r_viewport.GetHeight());
    set_scissor = false;
  }
  else if (AlmostEqual(r_viewport, r_projection))
  {
    result = QStringLiteral("VP+Proj 2D");
    if (r_viewport.left != 0 || r_viewport.top != 0)
      result += QStringLiteral(" (%1, %2)").arg(r_viewport.left).arg(r_viewport.top);
    result += QStringLiteral(" %1x%2, near %3 far %4")
                  .arg(r_viewport.GetWidth())
                  .arg(r_viewport.GetHeight())
                  .arg(near_z)
                  .arg(far_z);
    set_projection = false;
  }
  else if (set_viewport)
  {
    result = QStringLiteral("VP");
    if (r_viewport.left != 0 || r_viewport.top != 0)
      result += QStringLiteral(" (%1, %2)").arg(r_viewport.left).arg(r_viewport.top);
    result += QStringLiteral(" %1x%2").arg(r_viewport.GetWidth()).arg(r_viewport.GetHeight());
  }

  // Scissor
  if (set_scissor)
  {
    if (set_viewport)
      result += QStringLiteral(" ");
    if (set_scissor && set_projection && AlmostEqual(r_scissor, r_projection))
    {
      result += QStringLiteral("Scissor+Proj 2D");
      set_projection = false;
    }
    else if (set_scissor)
    {
      result += QStringLiteral("Scissor");
    }
    if (r_scissor.left != 0 || r_scissor.top != 0)
      result += QStringLiteral(" (%1, %2)").arg(r_scissor.left).arg(r_scissor.top);
    result += QStringLiteral(" %1x%2").arg(r_scissor.GetWidth()).arg(r_scissor.GetHeight());
    if (set_projection)
      result += QStringLiteral(", near %1 far %2").arg(near_z).arg(far_z);
  }

  // Projection
  if (set_projection)
  {
    if (set_viewport || set_scissor)
      result += QStringLiteral(" ");
    if (m_xfmem->projection.type == GX_ORTHOGRAPHIC)
    {
      result += QStringLiteral("Proj 2D");
      if (r_projection.left != 0 || r_projection.top != 0)
        result += QStringLiteral(" (%1, %2)").arg(r_projection.left).arg(r_projection.top);
      result += QStringLiteral(" %1x%2").arg(r_projection.GetWidth()).arg(r_projection.GetHeight());
      result += QStringLiteral(", near %4 far %5").arg(near_z).arg(far_z);
    }
    else
    {
      float h = m_xfmem->projection.rawProjection[0];
      float v = m_xfmem->projection.rawProjection[2];
      float aspect = v / h;
      float hfov = 2 * atan(1 / h);
      float vfov = 2 * atan(1 / v);
      float a = m_xfmem->projection.rawProjection[4];
      float b = m_xfmem->projection.rawProjection[5];
      near_z = b / (a - 1);
      far_z = b / a;
      result += QStringLiteral("FOV %1\302\260 x %2\302\260, AR 16:%3, near %4 far %5")
                    .arg(hfov * 360 / float(MathUtil::TAU))
                    .arg(vfov * 360 / float(MathUtil::TAU))
                    .arg(16 / aspect)
                    .arg(near_z)
                    .arg(far_z);
    }
  }
  if (min_depth != 0 || max_depth != 1)
  {
    result += QStringLiteral(", z %1 to %2").arg(min_depth).arg(max_depth);
  }
  return result;
}

QString FIFOAnalyzer::DescribeEFBCopy(QString* resolution)
{
  u32 destAddr = m_bpmem->copyTexDest << 5;
  u32 destStride = m_bpmem->copyMipMapStrideChannels << 5;

  MathUtil::Rectangle<int> srcRect;
  srcRect.left = static_cast<int>(m_bpmem->copyTexSrcXY.x);
  srcRect.top = static_cast<int>(m_bpmem->copyTexSrcXY.y);
  srcRect.right = static_cast<int>(m_bpmem->copyTexSrcXY.x + m_bpmem->copyTexSrcWH.x + 1);
  srcRect.bottom = static_cast<int>(m_bpmem->copyTexSrcXY.y + m_bpmem->copyTexSrcWH.y + 1);
  bool is_depth_copy = m_bpmem->zcontrol.pixel_format == PEControl::Z24;
  QString result;
  if (is_depth_copy)
    result = QStringLiteral("Depth ");
  const UPE_Copy PE_copy = m_bpmem->triggerEFBCopy;
  if (PE_copy.copy_to_xfb == 0)
  {
    result += QStringLiteral("Copy to Tex[%1 %2]").arg(destAddr, 0, 16).arg(destStride);
  }
  else
  {
    float yScale;
    if (PE_copy.scale_invert)
      yScale = 256.0f / static_cast<float>(m_bpmem->dispcopyyscale);
    else
      yScale = static_cast<float>(m_bpmem->dispcopyyscale) / 256.0f;

    float num_xfb_lines = 1.0f + m_bpmem->copyTexSrcWH.y * yScale;

    u32 height = static_cast<u32>(num_xfb_lines);

    result +=
        QStringLiteral("Copy to XFB[%1 %2x%3]").arg(destAddr, 0, 16).arg(destStride).arg(height);
  }
  QString res;
  if ((!AlmostEqual(srcRect.left, 0)) || (!AlmostEqual(srcRect.top, 0)))
    res += QStringLiteral(" (%1, %2)").arg(srcRect.left).arg(srcRect.top);
  res += QStringLiteral(" %1x%2").arg(srcRect.GetWidth()).arg(srcRect.GetHeight());
  result += res;
  if (resolution)
    *resolution = res;

  if (PE_copy.intensity_fmt)
    result += QStringLiteral(", Intensity");
  if (PE_copy.half_scale)
    result += QStringLiteral(", Half-scale");
  if (PE_copy.clamp_top)
    result += QStringLiteral(", Clamp top");
  if (PE_copy.clamp_bottom)
    result += QStringLiteral(", Clamp bottom");
  if (PE_copy.clear)
    result += QStringLiteral(", Clear");

  return result;
}

void FIFOAnalyzer::UpdateTree()
{
  m_tree_widget->clear();

  if (!FifoPlayer::GetInstance().IsPlaying())
  {
    auto* recording_item = new QTreeWidgetItem({tr("No recording loaded.")});
    recording_item->setData(0, TYPE_ROLE, TYPE_WHOLE);
    m_tree_widget->addTopLevelItem(recording_item);
    m_xfmem.reset();
    m_bpmem.reset();
    m_cpmem.reset();
    return;
  }

  QColor color;
  // projection/viewport changes will be blue
  color.setRgb(0, 80, 255);
  m_layer_brush.setColor(color);
  // scissor changes without a projection/viewport change will be green
  color.setRgb(10, 180, 0);
  m_scissor_brush.setColor(color);
  // all kinds of EFB copies (EFB copies/XFB copies/frames) will be red
  color.setRgb(200, 0, 0);
  m_efb_brush.setColor(color);

  auto* recording_item = new QTreeWidgetItem({tr("Recording")});
  recording_item->setData(0, TYPE_ROLE, TYPE_WHOLE);
  m_tree_widget->addTopLevelItem(recording_item);

  auto* file = FifoPlayer::GetInstance().GetFile();

  // keep track of the registers and which relevant ones have been modified
  {
    if (!m_xfmem)
      m_xfmem = std::make_unique<XFMemory>();
    if (!m_bpmem)
      m_bpmem = std::make_unique<BPMemory>();
    if (!m_cpmem)
      m_cpmem = std::make_unique<FifoAnalyzer::CPMemory>();
    u32* p = file->GetXFMem();
    memcpy(m_xfmem.get(), p, 0x1000 * sizeof(u32));
    p = file->GetXFRegs();
    memcpy(&m_xfmem->error, p, 0x58 * sizeof(u32));
    p = file->GetBPMem();
    memcpy(m_bpmem.get(), p, sizeof(BPMemory));
    p = file->GetCPMem();
    FifoAnalyzer::LoadCPReg(0x50, p[0x50], *(m_cpmem.get()));
    FifoAnalyzer::LoadCPReg(0x60, p[0x60], *(m_cpmem.get()));
    for (int i = 0; i < 8; ++i)
    {
      FifoAnalyzer::LoadCPReg(0x70 + i, p[0x70 + i], *(m_cpmem.get()));
      FifoAnalyzer::LoadCPReg(0x80 + i, p[0x80 + i], *(m_cpmem.get()));
      FifoAnalyzer::LoadCPReg(0x90 + i, p[0x90 + i], *(m_cpmem.get()));
    }
  }
  bool projection_set = false;
  bool viewport_set = false;
  bool scissor_set = false;
  bool scissor_offset_set = false;
  bool efb_copied = false;

  // loop through each frame and add it to the tree
  int frame_count = file->GetFrameCount();
  for (int frame_nr = 0; frame_nr < frame_count; frame_nr++)
  {
    auto* frame_item = new QTreeWidgetItem({tr("Frame %1").arg(frame_nr)});
    frame_item->setData(0, TYPE_ROLE, TYPE_FRAME);
    frame_item->setData(0, FRAME_ROLE, frame_nr);
    frame_item->setForeground(0, m_efb_brush);

    recording_item->addChild(frame_item);

    int layer = 0;
    int efbcopy_count = 0;

    const auto& frame_info = FifoPlayer::GetInstance().GetAnalyzedFrameInfo(frame_nr);
    int object_count = (int)frame_info.objectStarts.size();

    for (int object_nr = 0; object_nr < object_count + 1; object_nr++)
    {
      // add projection and viewport inherited from previous frame as layer 0
      if (object_nr == 0)
      {
        QString s = QStringLiteral("inherited: %1").arg(DescribeLayer(true, true, true));
        auto* layer_item = new QTreeWidgetItem({s});
        layer_item->setData(0, TYPE_ROLE, TYPE_INHERITED_LAYER);
        layer_item->setData(0, FRAME_ROLE, frame_nr);
        layer_item->setData(0, LAYER_ROLE, layer);
        layer_item->setForeground(0, m_layer_brush);
        frame_item->addChild(layer_item);
        layer++;
      }

      QString obj_desc;
      CheckObject(frame_nr, object_nr, m_xfmem.get(), m_bpmem.get(), &projection_set, &viewport_set,
                  &scissor_set, &scissor_offset_set, &efb_copied, &obj_desc);
      if (efb_copied && object_nr < object_count)
      {
        QString efb_copy = DescribeEFBCopy();
        QString s = QStringLiteral("EFB Copy %1: %2").arg(efbcopy_count).arg(efb_copy);
        auto* efbcopy_item = new QTreeWidgetItem({s});
        efbcopy_item->setData(0, TYPE_ROLE, TYPE_EFBCOPY);
        efbcopy_item->setData(0, FRAME_ROLE, frame_nr);
        efbcopy_item->setData(0, EFBCOPY_ROLE, efbcopy_count);
        efbcopy_item->setForeground(0, m_efb_brush);
        QTreeWidgetItem* parent = frame_item;
        FoldLayer(parent);
        int first = parent->childCount() - 1;
        while (first > 0)
        {
          QTreeWidgetItem* item = parent->child(first);
          if (!item->data(0, EFBCOPY_ROLE).isNull())
            break;
          first--;
        }
        first++;
        while (first < parent->childCount())
        {
          efbcopy_item->addChild(parent->takeChild(first));
        }
        parent->addChild(efbcopy_item);
        // if we don't clear the screen after the EFB Copy, we should still be able to see what's
        // inside it so reflect that in our tree too
        efbcopy_item->setExpanded(!(m_bpmem->triggerEFBCopy.clear));

        efbcopy_count++;
      }
      if (scissor_offset_set)
      {
        scissor_set = true;
        viewport_set = true;
      }
      if (projection_set || viewport_set || scissor_set)
      {
        QString s = QStringLiteral("%1: %2").arg(layer).arg(
            DescribeLayer(viewport_set, scissor_set, projection_set));
        auto* layer_item = new QTreeWidgetItem({s});
        layer_item->setData(0, TYPE_ROLE, TYPE_LAYER);
        layer_item->setData(0, FRAME_ROLE, frame_nr);
        layer_item->setData(0, LAYER_ROLE, layer);
        if (viewport_set || projection_set)
          layer_item->setForeground(0, m_layer_brush);
        else
          layer_item->setForeground(0, m_scissor_brush);
        QTreeWidgetItem* parent = frame_item;
        FoldLayer(parent);
        parent->addChild(layer_item);
        layer++;
      }
      else if (object_nr == object_count)
      {
        FoldLayer(frame_item);
      }

      // add the object itself
      QTreeWidgetItem* object_item;
      if (object_nr == object_count)
      {
        QString resolution;
        QString efb_copy = DescribeEFBCopy(&resolution);
        object_item = new QTreeWidgetItem({tr("XFB Copy: %1").arg(efb_copy)});
        object_item->setData(0, TYPE_ROLE, TYPE_XFBCOPY);
        object_item->setForeground(0, m_efb_brush);
        frame_item->setText(0, QStringLiteral("Frame %1: %2").arg(frame_nr).arg(resolution));
      }
      else
      {
        QString adjectives = GetAdjectives();
        object_item = new QTreeWidgetItem(
            {tr("Object %1:\t%2  \t%3").arg(object_nr).arg(obj_desc).arg(adjectives)});
        object_item->setData(0, TYPE_ROLE, TYPE_OBJECT);
        object_item->setData(0, VERTEX_ROLE, m_cpmem->vtxDesc.Hex);
        for (int i = 0; i < 8; i++)
        {
          auto sizes = FifoAnalyzer::CalculateVertexElementSizes(i, *(m_cpmem.get()));
          int vertexSize = std::accumulate(sizes.begin(), sizes.begin() + 21, 0u);
          object_item->setData(0, VSIZE_ROLE0 + i, vertexSize);
        }
      }
      object_item->setData(0, FRAME_ROLE, frame_nr);
      object_item->setData(0, OBJECT_ROLE, object_nr);
      frame_item->addChild(object_item);
    }
  }
  recording_item->setExpanded(true);
}

void FIFOAnalyzer::FoldLayer(QTreeWidgetItem* parent)
{
  int first = parent->childCount() - 1;
  QTreeWidgetItem* first_item = nullptr;
  while (first >= 0)
  {
    first_item = parent->child(first);
    if (!first_item->data(0, EFBCOPY_ROLE).isNull())
      break;
    if (!first_item->data(0, LAYER_ROLE).isNull())
      break;
    first--;
  }
  first++;
  if (first_item && first_item->data(0, EFBCOPY_ROLE).isNull())
  {
    while (first < parent->childCount())
    {
      first_item->addChild(parent->takeChild(first));
    }
    // everything inside a layer can still be seen
    // so reflect that in our tree too
    first_item->setExpanded(true);
  }
}

QString FIFOAnalyzer::GetAdjectives()
{
  std::string a;
  if (m_bpmem->genMode.zfreeze)
    a += "zfreeze ";
  if (m_bpmem->genMode.flat_shading)
    a += "flat-shading? ";
  if (((m_bpmem->zmode.testenable) && (m_bpmem->zmode.func == ZMode::NEVER)) ||
      (m_bpmem->genMode.cullmode == GenMode::CULL_ALL))
    a += "not-drawn ";
  else if ((!m_bpmem->zmode.testenable) || (m_bpmem->zmode.func == ZMode::ALWAYS))
    a += "always-on-top ";
  if (m_bpmem->genMode.cullmode == GenMode::CULL_NONE)
    a += "double-sided ";
  else if (m_bpmem->genMode.cullmode == GenMode::CULL_FRONT)
    a += "backface ";
  if (m_bpmem->fog.c_proj_fsel.fsel)
    a += "fogged ";
  if (m_bpmem->blendmode.logicopenable)
    a += "logic-op ";
  bool alpha_blended =
      (m_bpmem->blendmode.blendenable && m_bpmem->blendmode.srcfactor == BlendMode::SRCALPHA &&
       m_bpmem->blendmode.dstfactor == BlendMode::INVSRCALPHA);
  bool additive =
      (m_bpmem->blendmode.blendenable && m_bpmem->blendmode.srcfactor == BlendMode::SRCALPHA &&
       m_bpmem->blendmode.dstfactor == BlendMode::ONE);
  bool full_additive =
      (m_bpmem->blendmode.blendenable && m_bpmem->blendmode.srcfactor == BlendMode::ONE &&
       m_bpmem->blendmode.dstfactor == BlendMode::ONE);
  if (alpha_blended)
    a += "alpha-blended ";
  else if (full_additive)
    a += "100%-additive ";
  else if (additive)
    a += "additive ";

  return QString::fromStdString(a);
}

int ItemsFirstObject(QTreeWidgetItem* item, bool allow_siblings = false)
{
  // if it's the entire frame or sequence, start at the beginning
  if (!item->data(0, TYPE_ROLE).isNull())
  {
    int type = item->data(0, TYPE_ROLE).toInt();
    if (type == TYPE_FRAME || type == TYPE_XFBCOPY || type == TYPE_WHOLE)
      return 0;
  }
  // if it's an object, problem solved
  if (!item->data(0, OBJECT_ROLE).isNull())
    return item->data(0, OBJECT_ROLE).toInt();
  // if it has children, try the first child
  int result = INT_MAX;
  if (item->childCount() > 0)
    result = ItemsFirstObject(item->child(0), true);
  if (result < INT_MAX)
    return result;
  // if it's a layer, and there are objects after it before the next layer
  // try the first object after it
  if (item->parent() && !item->data(0, LAYER_ROLE).isNull())
  {
    int index = item->parent()->indexOfChild(item);
    if (index + 1 < item->parent()->childCount())
    {
      QTreeWidgetItem* next_item = item->parent()->child(index + 1);
      if ((next_item->data(0, LAYER_ROLE).isNull() && next_item->data(0, EFBCOPY_ROLE).isNull()) ||
          allow_siblings)
        result = ItemsFirstObject(next_item, allow_siblings);
    }
  }
  // if it's an EFB copy, and there are objects before it that aren't an EFB copy
  // keep going back to the first object before it that isn't an EFB copy
  else if (item->parent() && !item->data(0, EFBCOPY_ROLE).isNull())
  {
    int index = item->parent()->indexOfChild(item);
    while (index - 1 >= 0)
    {
      QTreeWidgetItem* prev_item = item->parent()->child(index - 1);
      if (!prev_item->data(0, EFBCOPY_ROLE).isNull())
        break;
      index--;
    }
    QTreeWidgetItem* prev_item = item->parent()->child(index);
    if (prev_item != item)
      result = ItemsFirstObject(prev_item);
  }
  // either we found our first object, or we're returning INT_MAX
  return result;
}

int ItemsLastObject(QTreeWidgetItem* item)
{
  // if it's the entire frame or sequence, play the whole thing
  if (!item->data(0, TYPE_ROLE).isNull())
  {
    int type = item->data(0, TYPE_ROLE).toInt();
    if (type == TYPE_FRAME || type == TYPE_XFBCOPY || type == TYPE_WHOLE)
      return INT_MAX - 1;
  }
  // if it's an object, problem solved
  if (!item->data(0, OBJECT_ROLE).isNull())
    return item->data(0, OBJECT_ROLE).toInt();
  // if it has children, try the last child
  int result = -1;
  if (item->childCount() > 0)
    result = ItemsFirstObject(item->child(item->childCount() - 1));
  if (result >= 0)
    return result;
  // if it's a layer, and there are objects after it before the next layer
  // try the last object after it
  if (item->parent() && !item->data(0, LAYER_ROLE).isNull())
  {
    int index = item->parent()->indexOfChild(item);
    while (index + 1 < item->parent()->childCount())
    {
      QTreeWidgetItem* next_item = item->parent()->child(index + 1);
      if ((!next_item->data(0, LAYER_ROLE).isNull()) ||
          (!next_item->data(0, EFBCOPY_ROLE).isNull()))
        break;
      index = index + 1;
    }
    QTreeWidgetItem* final_good_item = item->parent()->child(index);
    if (final_good_item != item)
      result = ItemsFirstObject(final_good_item);
  }
  // if it's an EFB copy, and there are objects before it that aren't an EFB copy
  // get the previous one
  else if (item->parent() && !item->data(0, EFBCOPY_ROLE).isNull())
  {
    int index = item->parent()->indexOfChild(item);
    if (index - 1 >= 0)
    {
      QTreeWidgetItem* prev_item = item->parent()->child(index - 1);
      if (prev_item->data(0, EFBCOPY_ROLE).isNull())
        result = ItemsFirstObject(prev_item);
    }
  }
  // either we found our last object, or we're returning -1
  return result;
}

void FIFOAnalyzer::UpdateDetails()
{
  m_detail_list->clear();
  m_object_data_offsets.clear();

  auto items = m_tree_widget->selectedItems();

  if (items.isEmpty())
    return;

  // Only play the selected frame and selected objects in the game window
  int first_object = INT_MAX;
  int last_object = -1;
  int first_frame = INT_MAX;
  int last_frame = -1;
  for (int sel = 0; sel < items.count(); sel++)
  {
    if (!items[sel]->data(0, FRAME_ROLE).isNull())
    {
      int frame = items[sel]->data(0, FRAME_ROLE).toInt();
      if (frame < first_frame && frame >= 0)
        first_frame = frame;
      if (frame > last_frame && frame < INT_MAX)
        last_frame = frame;
    }
    else
    {
      first_frame = 0;
      last_frame = INT_MAX - 1;
    }
    int test = ItemsFirstObject(items[sel]);
    if (test < first_object && test >= 0)
      first_object = test;
    if (test > last_object && test < INT_MAX)
      last_object = test;
    test = ItemsLastObject(items[sel]);
    if (test < first_object && test >= 0)
      first_object = test;
    if (test > last_object && test < INT_MAX)
      last_object = test;
  }
  if (first_frame == INT_MAX)
    first_frame = 0;
  if (last_frame < 0)
    last_frame = -1;
  FifoPlayer& player = FifoPlayer::GetInstance();
  player.SetObjectRangeStart(first_object);
  player.SetObjectRangeEnd(last_object);
  player.SetFrameRangeStart(first_frame);
  player.SetFrameRangeEnd(last_frame);

  if (items[0]->data(0, OBJECT_ROLE).isNull() || items[0]->data(0, FRAME_ROLE).isNull())
  {
    m_entry_detail_browser->clear();
    return;
  }

  // Actual updating of details starts here

  int frame_nr = items[0]->data(0, FRAME_ROLE).toInt();
  int object_nr = items[0]->data(0, OBJECT_ROLE).toInt();

  const auto& frame_info = player.GetAnalyzedFrameInfo(frame_nr);
  const auto& fifo_frame = player.GetFile()->GetFrame(frame_nr);

  const u8* objectdata_start;
  const u8* objectdata_end;
  if (object_nr < frame_info.objectStarts.size())
  {
    objectdata_start = &fifo_frame.fifoData[frame_info.objectStarts[object_nr]];
    objectdata_end = &fifo_frame.fifoData[frame_info.objectEnds[object_nr]];
  }
  else
  {
    objectdata_start = &fifo_frame.fifoData[fifo_frame.fifoData.size()];
    objectdata_end = objectdata_start;
  }
  const std::ptrdiff_t obj_offset = objectdata_start - &fifo_frame.fifoData[0];

  const u8* prev_objectdata_end;
  if (object_nr <= 0)
    prev_objectdata_end = &fifo_frame.fifoData[0];
  else
    prev_objectdata_end = &fifo_frame.fifoData[frame_info.objectEnds[object_nr - 1]];

  QString new_label;
  std::string name, desc;
  int color;

  // Between prev_objectdata_end and objectdata_start, there are register setting commands
  const u8* objectdata = prev_objectdata_end;
  while (objectdata < objectdata_start)
  {
    m_object_data_offsets.push_back(objectdata - objectdata_start);
    int new_offset = objectdata - &fifo_frame.fifoData[0];
    color = 0;
    int command = *objectdata++;
    switch (command)
    {
    case OpcodeDecoder::GX_NOP:
      new_label = QStringLiteral("NOP");
      break;

    case 0x44:
      new_label = QStringLiteral("0x44");
      break;

    case OpcodeDecoder::GX_CMD_INVL_VC:
      new_label = QStringLiteral("GX_CMD_INVL_VC");
      break;

    case OpcodeDecoder::GX_LOAD_CP_REG:
    {
      u32 cmd2 = *objectdata++;
      u32 value = Common::swap32(objectdata);
      objectdata += 4;

      new_label = QStringLiteral("CP  %1  %2")
                      .arg(cmd2, 2, 16, QLatin1Char('0'))
                      .arg(value, 8, 16, QLatin1Char('0'));
    }
    break;

    case OpcodeDecoder::GX_LOAD_XF_REG:
    {
      color = GetXFTransferInfo(objectdata, &name, &desc);
      u32 cmd2 = Common::swap32(objectdata);
      objectdata += 4;

      u8 streamSize = ((cmd2 >> 16) & 15) + 1;

      const u8* stream_start = objectdata;
      const u8* stream_end = stream_start + streamSize * 4;

      new_label = QStringLiteral("XF  %1  ").arg(cmd2, 8, 16, QLatin1Char('0'));
      while (objectdata < stream_end)
      {
        new_label += QStringLiteral("%1").arg(*objectdata++, 2, 16, QLatin1Char('0'));

        if (((objectdata - stream_start) % 4) == 0)
          new_label += QLatin1Char(' ');
      }
      new_label += QStringLiteral("    ");
      new_label += QString::fromStdString(name);
    }
    break;

    case OpcodeDecoder::GX_LOAD_INDX_A:
    case OpcodeDecoder::GX_LOAD_INDX_B:
    case OpcodeDecoder::GX_LOAD_INDX_C:
    case OpcodeDecoder::GX_LOAD_INDX_D:
    {
      objectdata += 4;
      new_label = (command == OpcodeDecoder::GX_LOAD_INDX_A) ?
                      QStringLiteral("LOAD INDX A") :
                      (command == OpcodeDecoder::GX_LOAD_INDX_B) ?
                      QStringLiteral("LOAD INDX B") :
                      (command == OpcodeDecoder::GX_LOAD_INDX_C) ? QStringLiteral("LOAD INDX C") :
                                                                   QStringLiteral("LOAD INDX D");
    }
    break;

    case OpcodeDecoder::GX_CMD_CALL_DL:
      // The recorder should have expanded display lists into the fifo stream and skipped the
      // call to start them
      // That is done to make it easier to track where memory is updated
      ASSERT(false);
      objectdata += 8;
      new_label = QStringLiteral("CALL DL");
      break;

    case OpcodeDecoder::GX_LOAD_BP_REG:
    {
      color = GetBPRegInfo(objectdata, &name, &desc);
      u32 cmd2 = Common::swap32(objectdata);
      objectdata += 4;
      new_label = QStringLiteral("BP  %1 %2    ")
                      .arg(cmd2 >> 24, 2, 16, QLatin1Char('0'))
                      .arg(cmd2 & 0xFFFFFF, 6, 16, QLatin1Char('0'));
      new_label += QString::fromStdString(name);
    }
    break;

    default:
      new_label = tr("Unexpected 0x80 call? Aborting...");
      objectdata = static_cast<const u8*>(objectdata_start);
      break;
    }
    new_label = QStringLiteral("%1:  ").arg(new_offset, 8, 16, QLatin1Char('0')) + new_label;
    QListWidgetItem* item = new QListWidgetItem(new_label);
    switch (color)
    {
    case 1:
      item->setForeground(m_efb_brush);
      break;
    case 2:
      item->setForeground(m_scissor_brush);
      break;
    case 3:
      item->setForeground(m_layer_brush);
      break;
    }
    m_detail_list->addItem(item);
  }

  // Add details for the object itself
  objectdata = objectdata_start;
  while (objectdata < objectdata_end)
  {
    const u8* drawcall_start = objectdata;
    m_object_data_offsets.push_back(drawcall_start - objectdata_start);
    int cmd = *objectdata++;
    switch (cmd)
    {
    case OpcodeDecoder::GX_NOP:
      new_label = QStringLiteral("NOP");
      m_detail_list->addItem(new_label);
      continue;

    case 0x44:
      new_label = QStringLiteral("0x44");
      m_detail_list->addItem(new_label);
      continue;

    case OpcodeDecoder::GX_CMD_INVL_VC:
      new_label = QStringLiteral("GX_CMD_INVL_VC");
      m_detail_list->addItem(new_label);
      continue;
    }
    int stream_size = Common::swap16(objectdata);
    objectdata += 2;
    int vtx_attr_group = cmd & OpcodeDecoder::GX_VAT_MASK;  // Vertex loader index (0 - 7);
    int primitive = (cmd & OpcodeDecoder::GX_PRIMITIVE_MASK) >> OpcodeDecoder::GX_PRIMITIVE_SHIFT;
    size_t vertex_size = items[0]->data(0, VSIZE_ROLE0 + vtx_attr_group).toInt();

    new_label = QStringLiteral("%1:  %2 %3 loader%4 %5 verts\n")
                    .arg(obj_offset, 8, 16, QLatin1Char('0'))
                    .arg(cmd, 2, 16, QLatin1Char('0'))
                    .arg(QString::fromUtf8(primitive_names[primitive]))
                    .arg(vtx_attr_group)
                    .arg(stream_size, 4, 16, QLatin1Char('0'));

    const u8* vertex_start = objectdata;
    while (objectdata < vertex_start + (vertex_size * stream_size))
      new_label += QStringLiteral("%1").arg(*objectdata++, 2, 16, QLatin1Char('0'));

    m_detail_list->addItem(new_label);
  }
}

void FIFOAnalyzer::BeginSearch()
{
  QString search_str = m_search_edit->text();

  auto items = m_tree_widget->selectedItems();

  if (items.isEmpty() || items[0]->data(0, FRAME_ROLE).isNull())
    return;

  if (items[0]->data(0, OBJECT_ROLE).isNull())
  {
    m_search_label->setText(tr("Invalid search parameters (no object selected)"));
    return;
  }

  // TODO: Remove even string length limit
  if (search_str.length() % 2)
  {
    m_search_label->setText(tr("Invalid search string (only even string lengths supported)"));
    return;
  }

  const size_t length = search_str.length() / 2;

  std::vector<u8> search_val;

  for (size_t i = 0; i < length; i++)
  {
    const QString byte_str = search_str.mid(static_cast<int>(i * 2), 2);

    bool good;
    u8 value = byte_str.toUInt(&good, 16);

    if (!good)
    {
      m_search_label->setText(tr("Invalid search string (couldn't convert to number)"));
      return;
    }

    search_val.push_back(value);
  }

  m_search_results.clear();

  int frame_nr = items[0]->data(0, FRAME_ROLE).toInt();
  int object_nr = items[0]->data(0, OBJECT_ROLE).toInt();

  const AnalyzedFrameInfo& frame_info = FifoPlayer::GetInstance().GetAnalyzedFrameInfo(frame_nr);
  const FifoFrameInfo& fifo_frame = FifoPlayer::GetInstance().GetFile()->GetFrame(frame_nr);

  // TODO: Support searching through the last object...how do we know where the cmd data ends?
  // TODO: Support searching for bit patterns

  const auto* start_ptr = &fifo_frame.fifoData[frame_info.objectStarts[object_nr]];
  const auto* end_ptr = &fifo_frame.fifoData[frame_info.objectStarts[object_nr + 1]];

  for (const u8* ptr = start_ptr; ptr < end_ptr - length + 1; ++ptr)
  {
    if (std::equal(search_val.begin(), search_val.end(), ptr))
    {
      SearchResult result;
      result.frame = frame_nr;

      result.object = object_nr;
      result.cmd = 0;
      for (unsigned int cmd_nr = 1; cmd_nr < m_object_data_offsets.size(); ++cmd_nr)
      {
        if (ptr < start_ptr + m_object_data_offsets[cmd_nr])
        {
          result.cmd = cmd_nr - 1;
          break;
        }
      }
      m_search_results.push_back(result);
    }
  }

  ShowSearchResult(0);

  m_search_label->setText(
      tr("Found %1 results for \"%2\"").arg(m_search_results.size()).arg(search_str));
}

void FIFOAnalyzer::FindNext()
{
  int index = m_detail_list->currentRow();

  if (index == -1)
  {
    ShowSearchResult(0);
    return;
  }

  for (auto it = m_search_results.begin(); it != m_search_results.end(); ++it)
  {
    if (it->cmd > index)
    {
      ShowSearchResult(it - m_search_results.begin());
      return;
    }
  }
}

void FIFOAnalyzer::FindPrevious()
{
  int index = m_detail_list->currentRow();

  if (index == -1)
  {
    ShowSearchResult(m_search_results.size() - 1);
    return;
  }

  for (auto it = m_search_results.rbegin(); it != m_search_results.rend(); ++it)
  {
    if (it->cmd < index)
    {
      ShowSearchResult(m_search_results.size() - 1 - (it - m_search_results.rbegin()));
      return;
    }
  }
}

void FIFOAnalyzer::ShowSearchResult(size_t index)
{
  if (m_search_results.empty())
    return;

  if (index > m_search_results.size())
  {
    ShowSearchResult(m_search_results.size() - 1);
    return;
  }

  const auto& result = m_search_results[index];

  QTreeWidgetItem* object_item =
      m_tree_widget->topLevelItem(0)->child(result.frame)->child(result.object);

  m_tree_widget->setCurrentItem(object_item);
  m_detail_list->setCurrentRow(result.cmd);

  m_search_next->setEnabled(index + 1 < m_search_results.size());
  m_search_previous->setEnabled(index > 0);
}

void FIFOAnalyzer::UpdateDescription()
{
  m_entry_detail_browser->clear();

  auto items = m_tree_widget->selectedItems();

  if (items.isEmpty() || items[0]->data(0, OBJECT_ROLE).isNull())
    return;

  int frame_nr = items[0]->data(0, FRAME_ROLE).toInt();
  int object_nr = items[0]->data(0, OBJECT_ROLE).toInt();
  int entry_nr = m_detail_list->currentRow();

  const AnalyzedFrameInfo& frame = FifoPlayer::GetInstance().GetAnalyzedFrameInfo(frame_nr);
  const FifoFrameInfo& fifo_frame = FifoPlayer::GetInstance().GetFile()->GetFrame(frame_nr);

  const u8* cmddata;
  if (object_nr < frame.objectStarts.size())
    cmddata = &fifo_frame.fifoData[frame.objectStarts[object_nr]];
  else
    cmddata = &fifo_frame.fifoData[fifo_frame.fifoData.size()];
  cmddata += m_object_data_offsets[entry_nr];

  // TODO: Not sure whether we should bother translating the descriptions

  QString text;
  if (*cmddata == OpcodeDecoder::GX_LOAD_BP_REG)
  {
    std::string name;
    std::string desc;
    GetBPRegInfo(cmddata + 1, &name, &desc);

    text = tr("BP register ");
    text += name.empty() ?
                QStringLiteral("UNKNOWN_%1").arg(*(cmddata + 1), 2, 16, QLatin1Char('0')) :
                QString::fromStdString(name);
    text += QLatin1Char{'\n'};

    if (desc.empty())
      text += tr("No description available");
    else
      text += QString::fromStdString(desc);
  }
  else if (*cmddata == OpcodeDecoder::GX_LOAD_CP_REG)
  {
    text = tr("CP register ");
  }
  else if (*cmddata == OpcodeDecoder::GX_LOAD_XF_REG)
  {
    std::string name;
    std::string desc;
    GetXFTransferInfo(cmddata + 1, &name, &desc);
    text = name.empty() ?
               QStringLiteral("UNKNOWN_%1").arg(*(cmddata + 1), 2, 16, QLatin1Char('0')) :
               QString::fromStdString(name);
    text += QLatin1Char{'\n'};

    if (desc.empty())
      text += tr("No description available");
    else
      text += QString::fromStdString(desc);
  }
  else if (*cmddata == OpcodeDecoder::GX_CMD_UNKNOWN_METRICS)
  {
    text = tr("0x44 GX_CMD_UNKNOWN_METRICS\n"
              "zelda 4 swords calls it and checks the metrics registers after that");
  }
  else if (*cmddata == OpcodeDecoder::GX_CMD_INVL_VC)
  {
    text = tr("Invalidate Vertex Cache?");
  }
  else if (*cmddata == OpcodeDecoder::GX_UNKNOWN_RESET)
  {
    text = tr("0x01 GX_UNKNOWN_RESET\nDatel software uses this command");
  }
  else if (*cmddata == OpcodeDecoder::GX_NOP)
  {
    text = tr("does nothing");
  }
  else if (*cmddata == OpcodeDecoder::GX_LOAD_INDX_A)
  {
    text = tr("Set position matrices");
  }
  else if (*cmddata == OpcodeDecoder::GX_LOAD_INDX_B)
  {
    text = tr("Set normal matrices");
  }
  else if (*cmddata == OpcodeDecoder::GX_LOAD_INDX_C)
  {
    text = tr("Set post matrices");
  }
  else if (*cmddata == OpcodeDecoder::GX_LOAD_INDX_D)
  {
    text = tr("Set light matrices");
  }
  else
  {
    text = tr("No description available");
  }

  m_entry_detail_browser->setText(text);
}

void FIFOAnalyzer::CheckObject(int frame_nr, int object_nr, XFMemory* xf, BPMemory* bp,
                               bool* projection_set, bool* viewport_set, bool* scissor_set,
                               bool* scissor_offset_set, bool* efb_copied, QString* desc)
{
  *projection_set = false;
  *viewport_set = false;
  *scissor_set = false;
  *scissor_offset_set = false;
  *efb_copied = false;
  const auto& frame_info = FifoPlayer::GetInstance().GetAnalyzedFrameInfo(frame_nr);
  const auto& fifo_frame = FifoPlayer::GetInstance().GetFile()->GetFrame(frame_nr);

  const u8* objectdata_start;
  const u8* objectdata_end;
  if (object_nr < frame_info.objectStarts.size())
  {
    objectdata_start = &fifo_frame.fifoData[frame_info.objectStarts[object_nr]];
    objectdata_end = &fifo_frame.fifoData[frame_info.objectEnds[object_nr]];
  }
  else
  {
    objectdata_start = &fifo_frame.fifoData[fifo_frame.fifoData.size()];
    objectdata_end = objectdata_start;
  }
  const std::ptrdiff_t obj_offset = objectdata_start - &fifo_frame.fifoData[0];

  const u8* prev_objectdata_end;
  if (object_nr <= 0)
    prev_objectdata_end = &fifo_frame.fifoData[0];
  else
    prev_objectdata_end = &fifo_frame.fifoData[frame_info.objectEnds[object_nr - 1]];

  const u8* objectdata = prev_objectdata_end;

  // Between prev_objectdata_end and objectdata_start, there are register setting commands
  while (objectdata < objectdata_start)
  {
    m_object_data_offsets.push_back(objectdata - objectdata_start);
    int new_offset = objectdata - &fifo_frame.fifoData[frame_info.objectStarts[0]];
    new_offset = new_offset;
    int command = *objectdata++;
    switch (command)
    {
    case OpcodeDecoder::GX_NOP:
    case 0x44:
    case OpcodeDecoder::GX_CMD_INVL_VC:
      break;
    case OpcodeDecoder::GX_LOAD_CP_REG:
    {
      u32 cmd2 = *objectdata++;
      u32 value = Common::swap32(objectdata);
      objectdata += 4;
      FifoAnalyzer::LoadCPReg(cmd2, value, *(m_cpmem.get()));
      break;
    }

    case OpcodeDecoder::GX_LOAD_XF_REG:
    {
      SimulateXFTransfer(objectdata, xf, projection_set, viewport_set);
      u32 cmd2 = Common::swap32(objectdata);
      objectdata += 4;

      const u8 streamSize = ((cmd2 >> 16) & 15) + 1;
      const u8* stream_start = objectdata;
      const u8* stream_end = stream_start + streamSize * 4;
      objectdata = stream_end;
      break;
    }

    case OpcodeDecoder::GX_LOAD_INDX_A:
    case OpcodeDecoder::GX_LOAD_INDX_B:
    case OpcodeDecoder::GX_LOAD_INDX_C:
    case OpcodeDecoder::GX_LOAD_INDX_D:
    {
      objectdata += 4;
      break;
    }

    case OpcodeDecoder::GX_CMD_CALL_DL:
      objectdata += 8;
      break;

    case OpcodeDecoder::GX_LOAD_BP_REG:
    {
      SimulateBPReg(objectdata, bp, scissor_set, scissor_offset_set, efb_copied);
      u32 cmd2 = Common::swap32(objectdata);
      cmd2 = cmd2;
      objectdata += 4;
    }
    break;

    default:
      objectdata = static_cast<const u8*>(objectdata_start);
      break;
    }
  }

  // Describe the draw calls
  *desc = QStringLiteral("");
  const char* prim_in_calls[] = {"quads", "quad2s", "tris",  "tris",
                                 "tris",  "lines",  "lines", "points"};
  const char* prim_calls[] = {"calls", "calls", "calls",  "strips",
                              "fans",  "calls", "strips", "calls"};
  // Keep track of previous similar draw calls so we can merge them
  QString prev_desc;
  int prev_prim = -1;
  int drawcall_count = 0;
  int total_prim_count = 0;
  int nop_count = 0;
  int broken_length = 0;
  objectdata = objectdata_start;
  while (objectdata < objectdata_end)
  {
    int cmd = *objectdata;
    objectdata++;
    nop_count = 0;
    // The FifoPlaybackAnalyzer includes these in with the object's draw calls
    while (((cmd == OpcodeDecoder::GX_NOP) || (cmd == OpcodeDecoder::GX_CMD_UNKNOWN_METRICS) ||
            (cmd == OpcodeDecoder::GX_CMD_INVL_VC)) &&
           objectdata <= objectdata_end)
    {
      nop_count++;
      cmd = *objectdata;
      objectdata++;
    }
    if (objectdata >= objectdata_end)
      continue;
    if ((cmd & 0xC0) != 0x80)
    {
      *desc += QStringLiteral(", Error! %1").arg(cmd, 2, 16, QLatin1Char('0'));
      break;
    }

    int stream_size = Common::swap16(objectdata);
    objectdata += 2;
    int primitive = (cmd & OpcodeDecoder::GX_PRIMITIVE_MASK) >> OpcodeDecoder::GX_PRIMITIVE_SHIFT;
    int count = stream_size;

    const std::array<int, 21> sizes = FifoAnalyzer::CalculateVertexElementSizes(
        cmd & OpcodeDecoder::GX_VAT_MASK, *(m_cpmem.get()));

    // Determine offset of each element that might be a vertex array
    // The first 9 elements are never vertex arrays so we just accumulate their sizes.
    int offset = std::accumulate(sizes.begin(), sizes.begin() + 9, 0u);
    std::array<int, 12> offsets;
    for (size_t i = 0; i < offsets.size(); ++i)
    {
      offsets[i] = offset;
      offset += sizes[i + 9];
    }

    const int vertexSize = offset;

    // We have something different now, so merge the previous similar things
    if (nop_count || primitive != prev_prim)
    {
      if ((*desc).length() - broken_length > LINE_LENGTH)
      {
        broken_length = (*desc).length();
        *desc += QStringLiteral(",\n");
      }
      else if (!(*desc).isEmpty())
      {
        *desc += QStringLiteral(", ");
      }
      if (drawcall_count == 1)
        *desc += prev_desc;
      else if (drawcall_count > 1)
        *desc += QStringLiteral("%1 %2 in %3 %4")
                     .arg(total_prim_count)
                     .arg(QString::fromUtf8(prim_in_calls[prev_prim]))
                     .arg(drawcall_count)
                     .arg(QString::fromUtf8(prim_calls[prev_prim]));
      if (nop_count)
      {
        if (drawcall_count > 0)
          *desc += QStringLiteral(", ");
        *desc += QStringLiteral("%1xNOP").arg(nop_count);
      }
      prev_desc.clear();
      prev_prim = 0;
      drawcall_count = 0;
      nop_count = 0;
    }
    drawcall_count++;
    prev_prim = primitive;
    prev_desc.clear();
    int prim_count = 0;
    switch (primitive)
    {
    case 0:
      prim_count = count / 4;
      if (count == 4)
        prev_desc = QStringLiteral("Quad (1 quad)");
      else
        prev_desc = QStringLiteral("%1 quads").arg(prim_count);
      break;
    case 1:
      prim_count = count / 4;
      if (count == 4)
        prev_desc = QStringLiteral("Quad (1 quad2)");
      else
        prev_desc = QStringLiteral("%1 quad2s").arg(prim_count);
      break;
    case 2:
      prim_count = count / 3;
      prev_desc = QStringLiteral("%1 tris").arg(prim_count);
      break;
    case 3:
      prim_count = count - 2;
      if (count == 4)
        prev_desc = QStringLiteral("Quad (2 tri-strip)");
      else if (count == 0)
        prev_desc = QStringLiteral("0 tri-strip");
      else
        prev_desc = QStringLiteral("%1 tri-strip").arg(prim_count);
      break;
    case 4:
      prim_count = count - 2;
      if (count == 4)
        prev_desc = QStringLiteral("Quad (2 fan)");
      else if (count == 0)
        prev_desc = QStringLiteral("0 fan");
      else
        prev_desc = QStringLiteral("%1 fan").arg(prim_count);
      break;
    case 5:
      prim_count = count / 2;
      prev_desc = QStringLiteral("%1 lines").arg(prim_count);
      break;
    case 6:
      prim_count = count - 1;
      if (count == 0)
        prev_desc = QStringLiteral("0 linestrip");
      else
        prev_desc = QStringLiteral("%1 linestrip").arg(prim_count);
      break;
    case 7:
      prim_count = count;
      prev_desc = QStringLiteral("%1 points").arg(prim_count);
      break;
    }
    if (!count)
      prim_count = 0;
    total_prim_count += prim_count;
    objectdata += (size_t)count * vertexSize;
  }
  if (nop_count || -1 != prev_prim)
  {
    if ((*desc).length() - broken_length > LINE_LENGTH)
    {
      broken_length = (*desc).length();
      *desc += QStringLiteral(",\n");
    }
    else if (!(*desc).isEmpty())
    {
      *desc += QStringLiteral(", ");
    }
    if (drawcall_count == 1)
      *desc += prev_desc;
    else if (drawcall_count > 1)
      *desc += QStringLiteral("%1 %2 in %3 %4")
                   .arg(total_prim_count)
                   .arg(QString::fromUtf8(prim_in_calls[prev_prim]))
                   .arg(drawcall_count)
                   .arg(QString::fromUtf8(prim_calls[prev_prim]));
    if (nop_count)
    {
      if (drawcall_count > 0)
        *desc += QStringLiteral(", ");
      *desc += QStringLiteral("%1xNOP").arg(nop_count);
    }
  }
}
