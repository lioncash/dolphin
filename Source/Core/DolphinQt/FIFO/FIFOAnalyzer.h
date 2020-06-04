// Copyright 2018 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#pragma once

#include <vector>

#include <QWidget>

class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSplitter;
class QTextBrowser;
class QTreeWidget;
class QTreeWidgetItem;

struct BPMemory;
struct Projection;
struct Viewport;
struct XFMemory;
namespace FifoAnalyzer
{
struct CPMemory;
}

class FIFOAnalyzer final : public QWidget
{
  Q_OBJECT

public:
  explicit FIFOAnalyzer();
  ~FIFOAnalyzer();

  void Update();
  QString DescribeLayer(bool set_viewport, bool set_scissor, bool set_projection);
  QString DescribeEFBCopy(QString* resolution = nullptr);

private:
  void CreateWidgets();
  void ConnectWidgets();

  void BeginSearch();
  void FindNext();
  void FindPrevious();

  void ShowSearchResult(size_t index);

  void UpdateTree();
  void FoldLayer(QTreeWidgetItem* parent);
  QString GetAdjectives();
  void UpdateDetails();
  void UpdateDescription();
  void CheckObject(int frame_nr, int object_nr, XFMemory* xf, BPMemory* bp, bool* projection_set,
                   bool* viewport_set, bool* scissor_set, bool* scissor_offset_set,
                   bool* efb_copied, QString* desc);

  QTreeWidget* m_tree_widget;
  QListWidget* m_detail_list;
  QTextBrowser* m_entry_detail_browser;
  QSplitter* m_object_splitter;

  // Search
  QGroupBox* m_search_box;
  QLineEdit* m_search_edit;
  QPushButton* m_search_new;
  QPushButton* m_search_next;
  QPushButton* m_search_previous;
  QLabel* m_search_label;
  QSplitter* m_search_splitter;

  QBrush m_layer_brush, m_scissor_brush, m_efb_brush;

  struct SearchResult
  {
    int frame;
    int object;
    int cmd;
  };

  std::vector<int> m_object_data_offsets;
  std::vector<SearchResult> m_search_results;
  std::unique_ptr<XFMemory> m_xfmem;
  std::unique_ptr<BPMemory> m_bpmem;
  std::unique_ptr<FifoAnalyzer::CPMemory> m_cpmem;
};
