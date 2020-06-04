// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include "DolphinQt/GameList/GameTracker.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>

#include "Core/ConfigManager.h"

#include "DiscIO/DirectoryBlob.h"

#include "DolphinQt/QtUtils/QueueOnObject.h"
#include "DolphinQt/QtUtils/RunOnObject.h"

#include "DolphinQt/Settings.h"

#include "UICommon/GameFile.h"

// NOTE: Qt likes to be case-sensitive here even though it shouldn't be thus this ugly regex hack
static const QStringList game_filters{
    QStringLiteral("*.[gG][cC][mM]"), QStringLiteral("*.[iI][sS][oO]"),
    QStringLiteral("*.[tT][gG][cC]"), QStringLiteral("*.[cC][iI][sS][oO]"),
    QStringLiteral("*.[gG][cC][zZ]"), QStringLiteral("*.[wW][bB][fF][sS]"),
    QStringLiteral("*.[wW][aA][dD]"), QStringLiteral("*.[eE][lL][fF]"),
    QStringLiteral("*.[dD][oO][lL]")};

GameTracker::GameTracker(QObject* parent) : QFileSystemWatcher(parent)
{
  qRegisterMetaType<std::shared_ptr<const UICommon::GameFile>>();
  qRegisterMetaType<std::string>();

  connect(this, &QFileSystemWatcher::directoryChanged, this, &GameTracker::UpdateDirectory);
  connect(this, &QFileSystemWatcher::fileChanged, this, &GameTracker::UpdateFile);
  connect(&Settings::Instance(), &Settings::AutoRefreshToggled, [] {
    const auto paths = Settings::Instance().GetPaths();

    for (const auto& path : paths)
    {
      Settings::Instance().RemovePath(path);
      Settings::Instance().AddPath(path);
    }
  });

  connect(&Settings::Instance(), &Settings::MetadataRefreshRequested, [this] {
    m_load_thread.EmplaceItem(Command{CommandType::UpdateMetadata, {}});
  });

  m_load_thread.Reset([this](Command command) {
    switch (command.type)
    {
    case CommandType::LoadCache:
      LoadCache();
      break;
    case CommandType::Start:
      StartInternal();
      break;
    case CommandType::AddDirectory:
      AddDirectoryInternal(command.path);
      break;
    case CommandType::RemoveDirectory:
      RemoveDirectoryInternal(command.path);
      break;
    case CommandType::UpdateDirectory:
      UpdateDirectoryInternal(command.path);
      break;
    case CommandType::UpdateFile:
      UpdateFileInternal(command.path);
      break;
    case CommandType::UpdateMetadata:
      m_cache.UpdateAdditionalMetadata(
          [this](const std::shared_ptr<const UICommon::GameFile>& game) {
            emit GameUpdated(game);
          });
      QueueOnObject(this, [] { Settings::Instance().NotifyMetadataRefreshComplete(); });
      break;
    case CommandType::PurgeCache:
      m_cache.Clear(UICommon::GameFileCache::DeleteOnDisk::Yes);
      break;
    case CommandType::BeginRefresh:
      if (m_busy_count++ == 0)
      {
        for (auto& file : m_tracked_files.keys())
          emit GameRemoved(file.toStdString());
        m_tracked_files.clear();
      }
      break;
    case CommandType::EndRefresh:
      if (--m_busy_count == 0)
      {
        QueueOnObject(this, [] { Settings::Instance().NotifyRefreshGameListComplete(); });
      }
      break;
    }
  });

  m_load_thread.EmplaceItem(Command{CommandType::LoadCache, {}});

  // TODO: When language changes, reload m_title_database and call m_cache.UpdateAdditionalMetadata
}

void GameTracker::LoadCache()
{
  m_cache.Load();
  m_cache_loaded_event.Set();
}

void GameTracker::Start()
{
  if (m_initial_games_emitted)
    return;

  m_initial_games_emitted = true;

  m_load_thread.EmplaceItem(Command{CommandType::Start, {}});

  m_cache_loaded_event.Wait();

  m_cache.ForEach(
      [this](const std::shared_ptr<const UICommon::GameFile>& game) { emit GameLoaded(game); });

  m_initial_games_emitted_event.Set();
}

void GameTracker::StartInternal()
{
  if (m_started)
    return;

  m_started = true;

  std::vector<std::string> paths;
  paths.reserve(m_tracked_files.size());
  for (const QString& path : m_tracked_files.keys())
    paths.push_back(path.toStdString());

  const auto emit_game_loaded = [this](const std::shared_ptr<const UICommon::GameFile>& game) {
    emit GameLoaded(game);
  };
  const auto emit_game_updated = [this](const std::shared_ptr<const UICommon::GameFile>& game) {
    emit GameUpdated(game);
  };
  const auto emit_game_removed = [this](const std::string& path) { emit GameRemoved(path); };

  m_initial_games_emitted_event.Wait();

  bool cache_updated = m_cache.Update(paths, emit_game_loaded, emit_game_removed);
  cache_updated |= m_cache.UpdateAdditionalMetadata(emit_game_updated);
  if (cache_updated)
    m_cache.Save();

  QueueOnObject(this, [] { Settings::Instance().NotifyMetadataRefreshComplete(); });
}

bool GameTracker::AddPath(const QString& dir)
{
  if (Settings::Instance().IsAutoRefreshEnabled())
    RunOnObject(this, [this, dir] { return addPath(dir); });

  m_tracked_paths.push_back(dir);

  return true;
}

bool GameTracker::RemovePath(const QString& dir)
{
  if (Settings::Instance().IsAutoRefreshEnabled())
    RunOnObject(this, [this, dir] { return removePath(dir); });

  const auto index = m_tracked_paths.indexOf(dir);

  if (index == -1)
    return false;

  m_tracked_paths.remove(index);

  return true;
}

void GameTracker::AddDirectory(const QString& dir)
{
  m_load_thread.EmplaceItem(Command{CommandType::AddDirectory, dir});
}

void GameTracker::RemoveDirectory(const QString& dir)
{
  m_load_thread.EmplaceItem(Command{CommandType::RemoveDirectory, dir});
}

void GameTracker::RefreshAll()
{
  m_load_thread.EmplaceItem(Command{CommandType::BeginRefresh});

  for (const QString& dir : Settings::Instance().GetPaths())
  {
    m_load_thread.EmplaceItem(Command{CommandType::RemoveDirectory, dir});
    m_load_thread.EmplaceItem(Command{CommandType::AddDirectory, dir});
  }

  m_load_thread.EmplaceItem(Command{CommandType::EndRefresh});
}

void GameTracker::UpdateDirectory(const QString& dir)
{
  m_load_thread.EmplaceItem(Command{CommandType::UpdateDirectory, dir});
}

void GameTracker::UpdateFile(const QString& dir)
{
  m_load_thread.EmplaceItem(Command{CommandType::UpdateFile, dir});
}

void GameTracker::AddDirectoryInternal(const QString& dir)
{
  if (!QFileInfo(dir).exists())
    return;
  AddPath(dir);
  UpdateDirectoryInternal(dir);
}

static std::unique_ptr<QDirIterator> GetIterator(const QString& dir)
{
  return std::make_unique<QDirIterator>(dir, game_filters, QDir::NoFilter,
                                        SConfig::GetInstance().m_RecursiveISOFolder ?
                                            QDirIterator::Subdirectories :
                                            QDirIterator::NoIteratorFlags);
}

void GameTracker::RemoveDirectoryInternal(const QString& dir)
{
  RemovePath(dir);
  auto it = GetIterator(dir);
  while (it->hasNext())
  {
    QString path = QFileInfo(it->next()).canonicalFilePath();
    if (m_tracked_files.contains(path))
    {
      m_tracked_files[path].remove(dir);
      if (m_tracked_files[path].empty())
      {
        RemovePath(path);
        m_tracked_files.remove(path);
        if (m_started)
          emit GameRemoved(path.toStdString());
      }
    }
  }
}

void GameTracker::UpdateDirectoryInternal(const QString& dir)
{
  auto it = GetIterator(dir);
  while (it->hasNext())
  {
    QString path = QFileInfo(it->next()).canonicalFilePath();

    if (m_tracked_files.contains(path))
    {
      auto& tracked_file = m_tracked_files[path];
      if (!tracked_file.contains(dir))
        tracked_file.insert(dir);
    }
    else
    {
      AddPath(path);
      m_tracked_files[path] = QSet<QString>{dir};
      LoadGame(path);
    }
  }

  for (const auto& missing : FindMissingFiles(dir))
  {
    auto& tracked_file = m_tracked_files[missing];

    tracked_file.remove(dir);
    if (tracked_file.empty())
    {
      m_tracked_files.remove(missing);
      if (m_started)
        GameRemoved(missing.toStdString());
    }
  }
}

void GameTracker::UpdateFileInternal(const QString& file)
{
  if (QFileInfo(file).exists())
  {
    if (m_started)
      GameRemoved(file.toStdString());
    AddPath(file);
    LoadGame(file);
  }
  else if (RemovePath(file))
  {
    m_tracked_files.remove(file);
    if (m_started)
      emit GameRemoved(file.toStdString());
  }
}

QSet<QString> GameTracker::FindMissingFiles(const QString& dir)
{
  auto it = GetIterator(dir);

  QSet<QString> missing_files;

  for (const auto& key : m_tracked_files.keys())
  {
    if (m_tracked_files[key].contains(dir))
      missing_files.insert(key);
  }

  while (it->hasNext())
  {
    QString path = QFileInfo(it->next()).canonicalFilePath();
    if (m_tracked_files.contains(path))
      missing_files.remove(path);
  }

  return missing_files;
}

void GameTracker::LoadGame(const QString& path)
{
  if (!m_started)
    return;

  const std::string converted_path = path.toStdString();
  if (!DiscIO::ShouldHideFromGameList(converted_path))
  {
    bool cache_changed = false;
    auto game = m_cache.AddOrGet(converted_path, &cache_changed);
    if (game)
      emit GameLoaded(std::move(game));
    if (cache_changed)
      m_cache.Save();
  }
}

void GameTracker::PurgeCache()
{
  m_load_thread.EmplaceItem(Command{CommandType::PurgeCache, {}});
  RefreshAll();
}
