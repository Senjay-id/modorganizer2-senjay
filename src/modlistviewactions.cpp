#include "modlistviewactions.h"

#include <QGridLayout>
#include <QGroupBox>
#include <QInputDialog>
#include <QLabel>
#include <QScrollArea>

#include "filesystemutilities.h"
#include <log.h>
#include <report.h>

#include "categories.h"
#include "csvbuilder.h"
#include "directoryrefresher.h"
#include "downloadmanager.h"
#include "filedialogmemory.h"
#include "filterlist.h"
#include "listdialog.h"
#include "messagedialog.h"
#include "modelutils.h"
#include "modinfodialog.h"
#include "modlist.h"
#include "modlistview.h"
#include "nexusinterface.h"
#include "nxmaccessmanager.h"
#include "organizercore.h"
#include "overwriteinfodialog.h"
#include "pluginlistview.h"
#include "savetextasdialog.h"
#include "shared/directoryentry.h"
#include "shared/fileregister.h"
#include "shared/filesorigin.h"

using namespace MOBase;
using namespace MOShared;

ModListViewActions::ModListViewActions(OrganizerCore& core, FilterList& filters,
                                       CategoryFactory& categoryFactory,
                                       ModListView* view, PluginListView* pluginView,
                                       QObject* nxmReceiver)
    : QObject(view), m_core(core), m_filters(filters), m_categories(categoryFactory),
      m_view(view), m_pluginView(pluginView), m_parent(view->topLevelWidget()),
      m_receiver(nxmReceiver)
{}

int ModListViewActions::findInstallPriority(const QModelIndex& index) const
{
  int newPriority = -1;
  if (index.isValid() && index.data(ModList::IndexRole).isValid() &&
      m_view->sortColumn() == ModList::COL_PRIORITY) {
    auto mIndex = index.data(ModList::IndexRole).toInt();
    auto info   = ModInfo::getByIndex(mIndex);
    newPriority = m_core.currentProfile()->getModPriority(mIndex);
    if (info->isSeparator()) {

      auto isSeparator = [](const auto& p) {
        return ModInfo::getByIndex(p.second)->isSeparator();
      };

      auto& ibp = m_core.currentProfile()->getAllIndexesByPriority();

      // start right after/before the current priority and look for the next
      // separator
      if (m_view->sortOrder() == Qt::AscendingOrder) {
        auto it = std::find_if(ibp.find(newPriority + 1), ibp.end(), isSeparator);
        if (it != ibp.end()) {
          newPriority = it->first;
        } else {
          newPriority = -1;
        }
      } else {
        auto it = std::find_if(std::reverse_iterator{ibp.find(newPriority - 1)},
                               ibp.rend(), isSeparator);
        if (it != ibp.rend()) {
          newPriority = it->first + 1;
        } else {
          // create "before" priority 0, i.e. at the end in descending priority.
          newPriority = 0;
        }
      }
    }
  }

  return newPriority;
}

void ModListViewActions::installMod(const QString& archivePath,
                                    const QModelIndex& index) const
{
  try {
    QString path = archivePath;
    if (path.isEmpty()) {
      QStringList extensions = m_core.installationManager()->getSupportedExtensions();
      for (auto iter = extensions.begin(); iter != extensions.end(); ++iter) {
        *iter = "*." + *iter;
      }

      path = FileDialogMemory::getOpenFileName(
          "installMod", m_parent, tr("Choose Mod"), QString(),
          tr("Mod Archive").append(QString(" (%1)").arg(extensions.join(" "))));
    }

    if (path.isEmpty()) {
      return;
    } else {
      m_core.installMod(path, findInstallPriority(index), false, nullptr, QString());
    }
  } catch (const std::exception& e) {
    reportError(e.what());
  }
}

void ModListViewActions::createEmptyMod(const QModelIndex& index) const
{
  GuessedValue<QString> name;
  name.setFilter(&fixDirectoryName);

  while (name->isEmpty()) {
    bool ok;
    name.update(QInputDialog::getText(m_parent, tr("Create Mod..."),
                                      tr("This will create an empty mod.\n"
                                         "Please enter a name:"),
                                      QLineEdit::Normal, "", &ok),
                GUESS_USER);
    if (!ok) {
      return;
    }
  }

  if (m_core.modList()->getMod(name) != nullptr) {
    reportError(tr("A mod with this name already exists"));
    return;
  }

  if (m_core.createMod(name) == nullptr) {
    return;
  }

  // find the priority before refresh() otherwise the index might not be valid
  const int newPriority = findInstallPriority(index);
  m_core.refresh();

  const auto mIndex = ModInfo::getIndex(name);
  if (newPriority >= 0) {
    m_core.modList()->changeModPriority(mIndex, newPriority);
  }

  m_view->scrollToAndSelect(
      m_view->indexModelToView(m_core.modList()->index(mIndex, 0)));
}

void ModListViewActions::createSeparator(const QModelIndex& index) const
{
  GuessedValue<QString> name;
  name.setFilter(&fixDirectoryName);
  while (name->isEmpty()) {
    bool ok;
    name.update(QInputDialog::getText(m_parent, tr("Create Separator..."),
                                      tr("This will create a new separator.\n"
                                         "Please enter a name:"),
                                      QLineEdit::Normal, "", &ok),
                GUESS_USER);
    if (!ok) {
      return;
    }
  }
  if (m_core.modList()->getMod(name) != nullptr) {
    reportError(tr("A separator with this name already exists"));
    return;
  }
  name->append("_separator");
  if (m_core.modList()->getMod(name) != nullptr) {
    return;
  }

  int newPriority = -1;
  if (index.isValid() && m_view->sortColumn() == ModList::COL_PRIORITY) {
    newPriority =
        m_core.currentProfile()->getModPriority(index.data(ModList::IndexRole).toInt());

    // descending order, we need to fix the priority
    if (m_view->sortOrder() == Qt::DescendingOrder) {
      newPriority++;
    }
  }

  if (m_core.createMod(name) == nullptr) {
    return;
  }

  m_core.refresh();

  const auto mIndex = ModInfo::getIndex(name);
  if (newPriority >= 0) {
    m_core.modList()->changeModPriority(mIndex, newPriority);
  }

  if (auto c = m_core.settings().colors().previousSeparatorColor()) {
    ModInfo::getByIndex(mIndex)->setColor(*c);
  }

  m_view->scrollToAndSelect(
      m_view->indexModelToView(m_core.modList()->index(mIndex, 0)));
}

void ModListViewActions::setAllMatchingModsEnabled(bool enabled) const
{
  // number of mods to enable / disable
  const auto counters = m_view->counters();
  const auto count    = enabled ? counters.visible.regular - counters.visible.active
                                : counters.visible.active;

  // retrieve visible mods from the model view
  const auto allIndex = m_view->indexViewToModel(flatIndex(m_view->model()));
  const QString message =
      enabled ? tr("Really enable %1 mod(s)?") : tr("Really disable %1 mod(s)?");
  if (QMessageBox::question(m_parent, tr("Confirm"), message.arg(count),
                            QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes) {
    m_core.modList()->setActive(allIndex, enabled);
  }
}

void ModListViewActions::checkModsForUpdates() const
{
  bool checkingModsForUpdate = false;
  if (NexusInterface::instance().getAccessManager()->validated()) {
    checkingModsForUpdate =
        ModInfo::checkAllForUpdate(&m_core.pluginContainer(), m_receiver);
    NexusInterface::instance().requestEndorsementInfo(m_receiver, QVariant(),
                                                      QString());
    NexusInterface::instance().requestTrackingInfo(m_receiver, QVariant(), QString());
  } else {
    QString apiKey;
    if (GlobalSettings::nexusApiKey(apiKey)) {
      m_core.doAfterLogin([=]() {
        checkModsForUpdates();
      });
      NexusInterface::instance().getAccessManager()->apiCheck(apiKey);
    } else {
      log::warn("{}", tr("You are not currently authenticated with Nexus. Please do so "
                         "under Settings -> Nexus."));
    }
  }

  bool updatesAvailable = false;
  for (auto mod : m_core.modList()->allMods()) {
    ModInfo::Ptr modInfo = ModInfo::getByName(mod);
    if (modInfo->updateAvailable()) {
      updatesAvailable = true;
      break;
    }
  }

  if (updatesAvailable || checkingModsForUpdate) {
    m_view->setFilterCriteria(
        {{ModListSortProxy::TypeSpecial, CategoryFactory::UpdateAvailable, false}});

    m_filters.setSelection(
        {{ModListSortProxy::TypeSpecial, CategoryFactory::UpdateAvailable, false}});
  }
}

void ModListViewActions::assignCategories() const
{
  if (!GlobalSettings::hideAssignCategoriesQuestion()) {
    QMessageBox warning;
    warning.setWindowTitle(tr("Are you sure?"));
    warning.setText(
        tr("This action will remove any existing categories on any mod with a valid "
           "Nexus category mapping. Are you certain you want to proceed?"));
    warning.setStandardButtons(QMessageBox::Yes | QMessageBox::Cancel);
    QCheckBox dontShow(tr("&Don't show this again"));
    warning.setCheckBox(&dontShow);
    auto result = warning.exec();
    if (dontShow.isChecked())
      GlobalSettings::setHideAssignCategoriesQuestion(true);
    if (result == QMessageBox::Cancel)
      return;
  }
  for (auto mod : m_core.modList()->allMods()) {
    ModInfo::Ptr modInfo = ModInfo::getByName(mod);
    if (modInfo->isSeparator())
      continue;
    int nexusCategory = modInfo->getNexusCategory();
    if (!nexusCategory) {
      QSettings downloadMeta(m_core.downloadsPath() + "/" +
                                 modInfo->installationFile() + ".meta",
                             QSettings::IniFormat);
      if (downloadMeta.contains("category")) {
        nexusCategory = downloadMeta.value("category", 0).toInt();
      }
    }
    int newCategory = CategoryFactory::instance().resolveNexusID(nexusCategory);
    if (newCategory != 0) {
      for (auto category : modInfo->categories()) {
        modInfo->removeCategory(category);
      }
    }
    modInfo->setCategory(CategoryFactory::instance().getCategoryID(newCategory), true);
  }
}

void ModListViewActions::checkModsForUpdates(
    std::multimap<QString, int> const& IDs) const
{
  if (m_core.settings().network().offlineMode()) {
    return;
  }

  if (NexusInterface::instance().getAccessManager()->validated()) {
    ModInfo::manualUpdateCheck(m_receiver, IDs);
  } else {
    QString apiKey;
    if (GlobalSettings::nexusApiKey(apiKey)) {
      m_core.doAfterLogin([=]() {
        checkModsForUpdates(IDs);
      });
      NexusInterface::instance().getAccessManager()->apiCheck(apiKey);
    } else
      log::warn("{}", tr("You are not currently authenticated with Nexus. Please do so "
                         "under Settings -> Nexus."));
  }
}

void ModListViewActions::checkModsForUpdates(const QModelIndexList& indices) const
{
  std::multimap<QString, int> ids;
  for (auto& idx : indices) {
    ModInfo::Ptr info = ModInfo::getByIndex(idx.data(ModList::IndexRole).toInt());
    ids.insert(std::make_pair<QString, int>(info->gameName(), info->nexusId()));
  }
  checkModsForUpdates(ids);
}

void ModListViewActions::exportModListCSV() const
{
  QDialog selection(m_parent);
  QGridLayout* grid = new QGridLayout;
  selection.setWindowTitle(tr("Export to csv"));

  QLabel* csvDescription = new QLabel();
  csvDescription->setText(
      tr("CSV (Comma Separated Values) is a format that can be imported in programs "
         "like Excel to create a spreadsheet.\nYou can also use online editors and "
         "converters instead."));
  grid->addWidget(csvDescription);

  QGroupBox* groupBoxRows = new QGroupBox(tr("Select what mods you want export:"));
  QRadioButton* all       = new QRadioButton(tr("All installed mods"));
  QRadioButton* active =
      new QRadioButton(tr("Only active (checked) mods from your current profile"));
  QRadioButton* visible =
      new QRadioButton(tr("All currently visible mods in the mod list"));

  QVBoxLayout* vbox = new QVBoxLayout;
  vbox->addWidget(all);
  vbox->addWidget(active);
  vbox->addWidget(visible);
  vbox->addStretch(1);
  groupBoxRows->setLayout(vbox);

  grid->addWidget(groupBoxRows);

  QButtonGroup* buttonGroupRows = new QButtonGroup();
  buttonGroupRows->addButton(all, 0);
  buttonGroupRows->addButton(active, 1);
  buttonGroupRows->addButton(visible, 2);
  buttonGroupRows->button(0)->setChecked(true);

  QGroupBox* groupBoxColumns = new QGroupBox(tr("Choose what Columns to export:"));
  groupBoxColumns->setFlat(true);

  QCheckBox* mod_Priority = new QCheckBox(tr("Mod_Priority"));
  mod_Priority->setChecked(true);
  QCheckBox* mod_Name = new QCheckBox(tr("Mod_Name"));
  mod_Name->setChecked(true);
  QCheckBox* mod_Note   = new QCheckBox(tr("Notes_column"));
  QCheckBox* mod_Status = new QCheckBox(tr("Mod_Status"));
  mod_Status->setChecked(true);
  QCheckBox* primary_Category   = new QCheckBox(tr("Primary_Category"));
  QCheckBox* mod_Author         = new QCheckBox(tr("Mod_Author"));
  QCheckBox* mod_Uploader       = new QCheckBox(tr("Mod_Uploader"));
  QCheckBox* nexus_ID           = new QCheckBox(tr("Nexus_ID"));
  QCheckBox* mod_Nexus_URL      = new QCheckBox(tr("Mod_Nexus_URL"));
  QCheckBox* mod_Uploader_URL   = new QCheckBox(tr("Mod_Uploader_URL"));
  QCheckBox* mod_Version        = new QCheckBox(tr("Mod_Version"));
  QCheckBox* install_Date       = new QCheckBox(tr("Install_Date"));
  QCheckBox* download_File_Name = new QCheckBox(tr("Download_File_Name"));

  QVBoxLayout* vbox1 = new QVBoxLayout;
  vbox1->addWidget(mod_Priority);
  vbox1->addWidget(mod_Name);
  vbox1->addWidget(mod_Status);
  vbox1->addWidget(mod_Note);
  vbox1->addWidget(primary_Category);
  vbox1->addWidget(mod_Author);
  vbox1->addWidget(mod_Uploader);
  vbox1->addWidget(nexus_ID);
  vbox1->addWidget(mod_Nexus_URL);
  vbox1->addWidget(mod_Uploader_URL);
  vbox1->addWidget(mod_Version);
  vbox1->addWidget(install_Date);
  vbox1->addWidget(download_File_Name);
  groupBoxColumns->setLayout(vbox1);

  grid->addWidget(groupBoxColumns);

  QPushButton* ok     = new QPushButton("Ok");
  QPushButton* cancel = new QPushButton("Cancel");
  QDialogButtonBox* buttons =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);

  connect(buttons, SIGNAL(accepted()), &selection, SLOT(accept()));
  connect(buttons, SIGNAL(rejected()), &selection, SLOT(reject()));

  grid->addWidget(buttons);

  selection.setLayout(grid);

  if (selection.exec() == QDialog::Accepted) {

    unsigned int numMods = ModInfo::getNumMods();
    int selectedRowID    = buttonGroupRows->checkedId();

    try {
      QBuffer buffer;
      buffer.open(QIODevice::ReadWrite);
      CSVBuilder builder(&buffer);
      builder.setEscapeMode(CSVBuilder::TYPE_STRING, CSVBuilder::QUOTE_ALWAYS);
      std::vector<std::pair<QString, CSVBuilder::EFieldType>> fields;
      if (mod_Priority->isChecked())
        fields.push_back(
            std::make_pair(QString("#Mod_Priority"), CSVBuilder::TYPE_STRING));
      if (mod_Status->isChecked())
        fields.push_back(
            std::make_pair(QString("#Mod_Status"), CSVBuilder::TYPE_STRING));
      if (mod_Name->isChecked())
        fields.push_back(std::make_pair(QString("#Mod_Name"), CSVBuilder::TYPE_STRING));
      if (mod_Note->isChecked())
        fields.push_back(std::make_pair(QString("#Note"), CSVBuilder::TYPE_STRING));
      if (primary_Category->isChecked())
        fields.push_back(
            std::make_pair(QString("#Primary_Category"), CSVBuilder::TYPE_STRING));
      if (mod_Author->isChecked())
        fields.push_back(
            std::make_pair(QString("#Mod_Author"), CSVBuilder::TYPE_STRING));
      if (mod_Uploader->isChecked())
        fields.push_back(
            std::make_pair(QString("#Mod_Uploader"), CSVBuilder::TYPE_STRING));
      if (nexus_ID->isChecked())
        fields.push_back(
            std::make_pair(QString("#Nexus_ID"), CSVBuilder::TYPE_INTEGER));
      if (mod_Nexus_URL->isChecked())
        fields.push_back(
            std::make_pair(QString("#Mod_Nexus_URL"), CSVBuilder::TYPE_STRING));
      if (mod_Uploader_URL->isChecked())
        fields.push_back(
            std::make_pair(QString("#Mod_Uploader_URL"), CSVBuilder::TYPE_STRING));
      if (mod_Version->isChecked())
        fields.push_back(
            std::make_pair(QString("#Mod_Version"), CSVBuilder::TYPE_STRING));
      if (install_Date->isChecked())
        fields.push_back(
            std::make_pair(QString("#Install_Date"), CSVBuilder::TYPE_STRING));
      if (download_File_Name->isChecked())
        fields.push_back(
            std::make_pair(QString("#Download_File_Name"), CSVBuilder::TYPE_STRING));

      builder.setFields(fields);

      builder.writeHeader();

      auto indexesByPriority = m_core.currentProfile()->getAllIndexesByPriority();
      for (auto& iter : indexesByPriority) {
        ModInfo::Ptr info = ModInfo::getByIndex(iter.second);
        bool enabled      = m_core.currentProfile()->modEnabled(iter.second);
        if ((selectedRowID == 1) && !enabled) {
          continue;
        } else if ((selectedRowID == 2) && !m_view->isModVisible(iter.second)) {
          continue;
        }
        std::vector<ModInfo::EFlag> flags = info->getFlags();
        if ((std::find(flags.begin(), flags.end(), ModInfo::FLAG_OVERWRITE) ==
             flags.end()) &&
            (std::find(flags.begin(), flags.end(), ModInfo::FLAG_BACKUP) ==
             flags.end())) {
          if (mod_Priority->isChecked())
            builder.setRowField("#Mod_Priority",
                                QString("%1").arg(iter.first, 4, 10, QChar('0')));
          if (mod_Status->isChecked())
            builder.setRowField("#Mod_Status", (enabled) ? "+" : "-");
          if (mod_Name->isChecked())
            builder.setRowField("#Mod_Name", info->name());
          if (mod_Note->isChecked())
            builder.setRowField("#Note",
                                QString("%1").arg(info->comments().remove(',')));
          if (primary_Category->isChecked())
            builder.setRowField(
                "#Primary_Category",
                (m_categories.categoryExists(info->primaryCategory()))
                    ? m_categories.getCategoryNameByID(info->primaryCategory())
                    : "");
          if (mod_Author->isChecked())
            builder.setRowField("#Mod_Author", info->author());
          if (mod_Uploader->isChecked())
            builder.setRowField("#Mod_Uploader", info->uploader());
          if (nexus_ID->isChecked())
            builder.setRowField("#Nexus_ID", info->nexusId());
          if (mod_Nexus_URL->isChecked())
            builder.setRowField("#Mod_Nexus_URL",
                                (info->nexusId() > 0)
                                    ? NexusInterface::instance().getModURL(
                                          info->nexusId(), info->gameName())
                                    : "");
          if (mod_Uploader_URL->isChecked())
            builder.setRowField("#Mod_Uploader_URL", info->uploaderUrl());
          if (mod_Version->isChecked())
            builder.setRowField("#Mod_Version", info->version().canonicalString());
          if (install_Date->isChecked())
            builder.setRowField("#Install_Date",
                                info->creationTime().toString("yyyy/MM/dd HH:mm:ss"));
          if (download_File_Name->isChecked())
            builder.setRowField("#Download_File_Name", info->installationFile());

          builder.writeRow();
        }
      }

      SaveTextAsDialog saveDialog(m_parent);
      saveDialog.setText(buffer.data());
      saveDialog.exec();
    } catch (const std::exception& e) {
      reportError(tr("export failed: %1").arg(e.what()));
    }
  }
}

void ModListViewActions::displayModInformation(const QString& modName,
                                               ModInfoTabIDs tab) const
{
  unsigned int index = ModInfo::getIndex(modName);
  if (index == UINT_MAX) {
    log::error("failed to resolve mod name {}", modName);
    return;
  }

  ModInfo::Ptr modInfo = ModInfo::getByIndex(index);
  displayModInformation(modInfo, index, tab);
}

void ModListViewActions::displayModInformation(unsigned int index,
                                               ModInfoTabIDs tab) const
{
  ModInfo::Ptr modInfo = ModInfo::getByIndex(index);
  displayModInformation(modInfo, index, tab);
}

void ModListViewActions::displayModInformation(ModInfo::Ptr modInfo,
                                               unsigned int modIndex,
                                               ModInfoTabIDs tab) const
{
  if (!m_core.modList()->modInfoAboutToChange(modInfo)) {
    log::debug("a different mod information dialog is open. If this is incorrect, "
               "please restart MO");
    return;
  }
  std::vector<ModInfo::EFlag> flags = modInfo->getFlags();
  if (std::find(flags.begin(), flags.end(), ModInfo::FLAG_OVERWRITE) != flags.end()) {
    QDialog* dialog = m_parent->findChild<QDialog*>("__overwriteDialog");
    try {
      if (dialog == nullptr) {
        dialog = new OverwriteInfoDialog(modInfo, m_core, m_parent);
        dialog->setObjectName("__overwriteDialog");
      } else {
        qobject_cast<OverwriteInfoDialog*>(dialog)->setModInfo(modInfo);
      }

      dialog->show();
      dialog->raise();
      dialog->activateWindow();
      connect(dialog, &QDialog::finished, [=]() {
        m_core.modList()->modInfoChanged(modInfo);
        dialog->deleteLater();
        m_core.refreshDirectoryStructure();
      });
    } catch (const std::exception& e) {
      reportError(tr("Failed to display overwrite dialog: %1").arg(e.what()));
    }
  } else {
    modInfo->saveMeta();

    ModInfoDialog dialog(m_core, m_core.pluginContainer(), modInfo, m_view, m_parent);
    connect(&dialog, &ModInfoDialog::originModified, this,
            &ModListViewActions::originModified);
    connect(&dialog, &ModInfoDialog::modChanged, [=](unsigned int index) {
      auto idx = m_view->indexModelToView(m_core.modList()->index(index, 0));
      m_view->selectionModel()->select(idx, QItemSelectionModel::ClearAndSelect |
                                                QItemSelectionModel::Rows);
      m_view->scrollTo(idx);
    });

    // Open the tab first if we want to use the standard indexes of the tabs.
    if (tab != ModInfoTabIDs::None) {
      dialog.selectTab(tab);
    }

    dialog.exec();

    modInfo->saveMeta();
    m_core.modList()->modInfoChanged(modInfo);
    emit modInfoDisplayed();
  }

  if (m_core.currentProfile()->modEnabled(modIndex) && !modInfo->isForeign()) {
    FilesOrigin& origin =
        m_core.directoryStructure()->getOriginByName(ToWString(modInfo->name()));
    origin.enable(false);

    if (m_core.directoryStructure()->originExists(ToWString(modInfo->name()))) {
      FilesOrigin& origin =
          m_core.directoryStructure()->getOriginByName(ToWString(modInfo->name()));
      origin.enable(false);
      QString path       = modInfo->absolutePath();
      QString modDataDir = m_core.managedGame()->modDataDirectory();
      path               = modDataDir.isEmpty() ? path : path + "/" + modDataDir;
      m_core.directoryRefresher()->addModToStructure(
          m_core.directoryStructure(), modInfo->name(),
          m_core.currentProfile()->getModPriority(modIndex), path,
          modInfo->stealFiles(), modInfo->archives());
      DirectoryRefresher::cleanStructure(m_core.directoryStructure());
      m_core.directoryStructure()->getFileRegister()->sortOrigins();
      m_core.refreshLists();
    }
  }
}

void ModListViewActions::sendModsToTop(const QModelIndexList& indexes) const
{
  m_core.modList()->changeModsPriority(indexes, Profile::MinimumPriority);
}

void ModListViewActions::sendModsToBottom(const QModelIndexList& indexes) const
{
  m_core.modList()->changeModsPriority(indexes, Profile::MaximumPriority);
}

void ModListViewActions::sendModsToPriority(const QModelIndexList& indexes) const
{
  bool ok;
  int priority = QInputDialog::getInt(m_parent, tr("Set Priority"),
                                      tr("Set the priority of the selected mods"), 0, 0,
                                      std::numeric_limits<int>::max(), 1, &ok);
  if (!ok)
    return;

  m_core.modList()->changeModsPriority(indexes, priority);
}

void ModListViewActions::sendModsToSeparator(const QModelIndexList& indexes) const
{
  QStringList separators;
  const auto& ibp = m_core.currentProfile()->getAllIndexesByPriority();
  for (const auto& [priority, index] : ibp) {
    if (index < ModInfo::getNumMods()) {
      ModInfo::Ptr modInfo = ModInfo::getByIndex(index);
      if (modInfo->isSeparator()) {
        separators << modInfo->name().chopped(
            10);  // chops the "_separator" away from the name
      }
    }
  }

  // in descending order, reverse the separator
  if (m_view->sortOrder() == Qt::DescendingOrder) {
    std::reverse(separators.begin(), separators.end());
  }

  ListDialog dialog(m_parent);
  dialog.setWindowTitle("Select a separator...");
  dialog.setChoices(separators);

  if (dialog.exec() != QDialog::Accepted) {
    return;
  }

  const QString result = dialog.getChoice();
  if (result.isEmpty()) {
    return;
  }

  const auto sepPriority =
      m_core.currentProfile()->getModPriority(ModInfo::getIndex(result + "_separator"));

  auto isSeparator = [](const auto& p) {
    return ModInfo::getByIndex(p.second)->isSeparator();
  };

  // start right after/before the current priority and look for the next
  // separator
  int priority = -1;
  if (m_view->sortOrder() == Qt::AscendingOrder) {
    auto it = std::find_if(ibp.find(sepPriority + 1), ibp.end(), isSeparator);
    if (it != ibp.end()) {
      priority = it->first;
    } else {
      priority = Profile::MaximumPriority;
    }
  } else {
    auto it = std::find_if(--std::reverse_iterator{ibp.find(sepPriority - 1)},
                           ibp.rend(), isSeparator);
    if (it != ibp.rend()) {
      priority = it->first + 1;
    } else {
      // create "before" priority 0, i.e. at the end in descending priority.
      priority = Profile::MinimumPriority;
    }
  }

  // when the priority of a single mod is incremented, we need to shift the
  // target priority, otherwise we will miss the target by one
  if (indexes.size() == 1 &&
      indexes[0].data(ModList::PriorityRole).toInt() < sepPriority) {
    priority--;
  }

  m_core.modList()->changeModsPriority(indexes, priority);
}

void ModListViewActions::sendModsToFirstConflict(const QModelIndexList& indexes) const
{
  std::set<unsigned int> conflicts;

  for (auto& idx : indexes) {
    if (!idx.data(ModList::IndexRole).isValid()) {
      continue;
    }
    auto info = ModInfo::getByIndex(idx.data(ModList::IndexRole).toInt());
    conflicts.insert(info->getModOverwrite().begin(), info->getModOverwrite().end());
  }

  std::set<int> priorities;
  std::transform(conflicts.begin(), conflicts.end(),
                 std::inserter(priorities, priorities.end()), [=](auto index) {
                   return m_core.currentProfile()->getModPriority(index);
                 });

  if (!priorities.empty()) {
    m_core.modList()->changeModsPriority(indexes, *priorities.begin());
  }
}

void ModListViewActions::sendModsToLastConflict(const QModelIndexList& indexes) const
{
  std::set<unsigned int> conflicts;

  for (auto& idx : indexes) {
    if (!idx.data(ModList::IndexRole).isValid()) {
      continue;
    }
    auto info = ModInfo::getByIndex(idx.data(ModList::IndexRole).toInt());
    conflicts.insert(info->getModOverwritten().begin(),
                     info->getModOverwritten().end());
  }

  std::set<int> priorities;
  std::transform(conflicts.begin(), conflicts.end(),
                 std::inserter(priorities, priorities.end()), [=](auto index) {
                   return m_core.currentProfile()->getModPriority(index);
                 });

  if (!priorities.empty()) {
    m_core.modList()->changeModsPriority(indexes, *priorities.rbegin());
  }
}

void ModListViewActions::updateToNexus(const QModelIndex& index) const
{
  auto& settings = m_core.settings().interface();
  MOBase::TaskDialog dlg(nullptr);

  const auto r =
      dlg.title(tr("Update to nexus"))
          .main(tr("It seems like this is the first time your using this feature"))
          .content(tr("This feature is still experimental, the fork author will not take any responsibility if it somehow edited your mods incorrectly.\n\n"
            "The updater source code exists at MO2/mod_updater/update.py, MO2 only passes the metadata of the mod into a json file to be used by the script.\n\n"
            "Please meet these requirement before proceeding\n"
                      "1. Install 7z (installer version)\n"
                      "2. Install python and selenium library\n"
                      "3. Logged into nexus on microsoft edge\n"
                      "4. You are the author or have permission to this mod\n\n"
                      "This will zip all files inside this mod folder and "
                      "upload it as is excluding the meta.ini file"))
          .icon(QMessageBox::Question)
          .button({tr("OK"), QMessageBox::Ok})
          .remember("rememberUpdateToNexus")
          .exec();

  if (r != QMessageBox::Ok)
    return;

  if (!index.isValid()) {
    return;
  }

  ModInfo::Ptr modInfo = ModInfo::getByIndex(index.data(ModList::IndexRole).toInt());

  if (!modInfo || modInfo->isSeparator()) {
    return;
  }

  // Create the update dialog - make it modeless
  QDialog* dialog = new QDialog(m_parent);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowTitle(tr("Update Mod: %1").arg(modInfo->name()));
  dialog->setMinimumWidth(600);
  // dialog->setMinimumHeight(700);

  QVBoxLayout* mainLayout = new QVBoxLayout(dialog);

  // File Information Group
  QGroupBox* fileInfoGroup    = new QGroupBox(tr("File Information"), dialog);
  QGridLayout* fileInfoLayout = new QGridLayout(fileInfoGroup);

  // Filename field
  QLabel* filenameLabel   = new QLabel(tr("File name:"), fileInfoGroup);
  QLineEdit* filenameEdit = new QLineEdit(modInfo->name(), fileInfoGroup);
  filenameEdit->setPlaceholderText(tr("Enter file name..."));
  filenameEdit->setMaxLength(50);  // Added 50 character limit
  fileInfoLayout->addWidget(filenameLabel, 0, 0);
  fileInfoLayout->addWidget(filenameEdit, 0, 1);

  // File version field
  QLabel* versionLabel = new QLabel(tr("File Version:"), fileInfoGroup);
  QLineEdit* versionEdit =
      new QLineEdit(modInfo->version().displayString(1), fileInfoGroup);
  versionEdit->setPlaceholderText(tr("e.g., 1.0.0"));
  versionEdit->setMaxLength(50);  // Added 50 character limit
  fileInfoLayout->addWidget(versionLabel, 1, 0);
  fileInfoLayout->addWidget(versionEdit, 1, 1);

  // File description field
  QLabel* descriptionLabel   = new QLabel(tr("File Description:"), fileInfoGroup);
  QTextEdit* descriptionEdit = new QTextEdit(modInfo->fileDescription(), fileInfoGroup);
  descriptionEdit->setMaximumHeight(80);
  descriptionEdit->setPlaceholderText(tr("Enter file description..."));

  // Add character limit for description (255 characters)
  QLabel* descriptionLimitLabel =
      new QLabel(tr("Characters remaining: 255"), fileInfoGroup);
  descriptionLimitLabel->setAlignment(Qt::AlignRight);
  descriptionLimitLabel->setStyleSheet("QLabel { color: gray; font-size: 10px; }");

  // Connect text changed signal to update character counter
  auto updateDescriptionCounter = [=]() {
    int currentLength = descriptionEdit->toPlainText().length();
    int remaining     = 255 - currentLength;
    descriptionLimitLabel->setText(tr("Characters remaining: %1").arg(remaining));

    // Change color when approaching limit
    if (remaining < 10) {
      descriptionLimitLabel->setStyleSheet("QLabel { color: red; font-size: 10px; }");
    } else if (remaining < 50) {
      descriptionLimitLabel->setStyleSheet(
          "QLabel { color: orange; font-size: 10px; }");
    } else {
      descriptionLimitLabel->setStyleSheet("QLabel { color: gray; font-size: 10px; }");
    }
  };

  connect(descriptionEdit, &QTextEdit::textChanged, updateDescriptionCounter);
  updateDescriptionCounter();  // Initialize the counter

  fileInfoLayout->addWidget(descriptionLabel, 2, 0);
  fileInfoLayout->addWidget(descriptionEdit, 2, 1);
  fileInfoLayout->addWidget(descriptionLimitLabel, 3, 1);

  mainLayout->addWidget(fileInfoGroup);

  // Options Group
  QGroupBox* optionsGroup    = new QGroupBox(tr("Options"), dialog);
  QVBoxLayout* optionsLayout = new QVBoxLayout(optionsGroup);

  QCheckBox* latestVersionCheck =
      new QCheckBox(tr("This is the latest version of the mod (your main version will "
                       "be updated automatically)"),
                    optionsGroup);
  latestVersionCheck->setChecked(settings.modUpdateToNXMLatestVersion());

  QCheckBox* newVersionCheck = new QCheckBox(
      tr("This is a new version of an existing file (optional)"), optionsGroup);
  newVersionCheck->setChecked(settings.modUpdateToNXMNewVersion());

  QCheckBox* removePreviousVersionCheck = new QCheckBox(
      tr("Remove the previous version after this file has been successfully uploaded"),
      optionsGroup);
  removePreviousVersionCheck->setChecked(settings.modUpdateToNXMRemovePreviousVersion());

  // Connect the new version checkbox to enable/disable remove previous version
  auto updateRemovePreviousVersionState = [=]() {
    bool isNewVersion = newVersionCheck->isChecked();
    // removePreviousVersionCheck->setEnabled(isNewVersion);

    // Visual feedback - gray out when disabled
    if (!isNewVersion) {
      removePreviousVersionCheck->setStyleSheet("QCheckBox { color: gray; }");
      // Also uncheck it since it's not applicable
      removePreviousVersionCheck->setChecked(false);
    } else {
      removePreviousVersionCheck->setStyleSheet("");  // Reset to default
      removePreviousVersionCheck->setChecked(
          true);  // Re-check if it was previously checked
    }
  };

  // Connect the signal
  connect(newVersionCheck, &QCheckBox::stateChanged, updateRemovePreviousVersionState);

  // Initialize the state
  updateRemovePreviousVersionState();

  // Add first 3 options with spacing
  optionsLayout->addWidget(latestVersionCheck);
  optionsLayout->addSpacing(5);
  optionsLayout->addWidget(newVersionCheck);
  optionsLayout->addSpacing(5);
  optionsLayout->addWidget(removePreviousVersionCheck);

  // Add larger space between first 3 and last 4 options
  optionsLayout->addSpacing(30);

  QCheckBox* removeDownloadManagerCheck =
      new QCheckBox(tr("Remove the 'Download with manager' button"), optionsGroup);
  removeDownloadManagerCheck->setChecked(settings.modUpdateToNXMRemoveDownloadWithManager());

  QCheckBox* setFileAsMainVortexCheck =
      new QCheckBox(tr("Set the file as the main Vortex file"), optionsGroup);
  setFileAsMainVortexCheck->setChecked(settings.modUpdateToNXMSetAsMainVortex());

  QCheckBox* informRequirementsCheck =
      new QCheckBox(tr("Inform downloaders of this mod's requirements before they "
                       "attempt to download this file"),
                    optionsGroup);
  informRequirementsCheck->setChecked(settings.modUpdateToNXMInformDownloader());

  QCheckBox* automaticSaveCheck = new QCheckBox(
      tr("Automatically save the file after successfully uploading it"), optionsGroup);
  automaticSaveCheck->setChecked(settings.modUpdateToNXMAutoSaveFile());

  QCheckBox* updateModVersionMetaData = new QCheckBox(
      tr("Update the current mod version to the metadata on MO2 after successfully uploading it"),
      optionsGroup);
  updateModVersionMetaData->setChecked(settings.modUpdateToNXMUodateCurrentModVersionToMeta());

  // Add last 4 options with spacing
  optionsLayout->addWidget(removeDownloadManagerCheck);
  optionsLayout->addSpacing(5);
  optionsLayout->addWidget(setFileAsMainVortexCheck);
  optionsLayout->addSpacing(5);
  optionsLayout->addWidget(informRequirementsCheck);
  optionsLayout->addSpacing(30);
  optionsLayout->addWidget(automaticSaveCheck);
  optionsLayout->addSpacing(5);
  optionsLayout->addWidget(updateModVersionMetaData);
  optionsLayout->addSpacing(5);
  optionsLayout->addStretch(1);

  mainLayout->addWidget(optionsGroup);

  // Changelog Group
  QGroupBox* changelogGroup    = new QGroupBox(tr("Changelog"), dialog);
  QVBoxLayout* changelogLayout = new QVBoxLayout(changelogGroup);

  QTextEdit* changelogEdit = new QTextEdit(changelogGroup);
  changelogEdit->setMaximumHeight(175);
  changelogEdit->setMinimumHeight(110);
  changelogEdit->setPlaceholderText(
      tr("Please write one entry per line. Example:\n* Added 64-bit support\n* Added "
         "raytracing...\n\n\nLeave it empty to not include a changelog"));

  // Add line length monitoring for changelog
  QLabel* changelogLineLimitLabel = new QLabel(tr(""), changelogGroup);
  changelogLineLimitLabel->setAlignment(Qt::AlignRight);
  changelogLineLimitLabel->setStyleSheet("QLabel { color: gray; font-size: 10px; }");

  // Connect text changed signal to monitor line lengths
  auto updateChangelogLineMonitor = [=]() {
    QString text      = changelogEdit->toPlainText();
    QStringList lines = text.split('\n');

    bool hasLongLines = false;
    int longLineCount = 0;

    for (int i = 0; i < lines.size(); ++i) {
      if (lines[i].length() > 50) {
        hasLongLines = true;
        longLineCount++;
      }
    }

    if (hasLongLines) {
      changelogLineLimitLabel->setText(
          tr("%1 line(s) exceed 50 characters").arg(longLineCount));
      changelogLineLimitLabel->setStyleSheet("QLabel { color: red; font-size: 10px; }");
    } else {
      changelogLineLimitLabel->setText(tr("All lines within 50 character limit"));
      changelogLineLimitLabel->setStyleSheet(
          "QLabel { color: green; font-size: 10px; }");
    }
  };

  connect(changelogEdit, &QTextEdit::textChanged, updateChangelogLineMonitor);
  updateChangelogLineMonitor();  // Initialize the monitor

  changelogLayout->addWidget(changelogEdit);
  changelogLayout->addWidget(changelogLineLimitLabel);

  mainLayout->addWidget(changelogGroup);
  mainLayout->addStretch(1);

  // "Remember my options" checkbox at bottom left
  QHBoxLayout* rememberLayout = new QHBoxLayout();
  QCheckBox* rememberOptionsCheck =
      new QCheckBox(tr("Remember my options choice"), dialog);
  rememberOptionsCheck->setChecked(false);  // Default to unchecked for safety
  rememberLayout->addWidget(rememberOptionsCheck);
  rememberLayout->addStretch(1);  // Push to left
  mainLayout->addLayout(rememberLayout);

  // Function to update changelog accessibility based on automaticSaveCheck state
  auto updateChangelogAccessibility = [=]() {
    bool isAutosaveEnabled = automaticSaveCheck->isChecked();

    // Disable/enable the changelog edit field
    changelogEdit->setEnabled(isAutosaveEnabled);

    // Also disable/enable the entire changelog group for visual consistency
    changelogGroup->setEnabled(isAutosaveEnabled);

    // Update the placeholder text to indicate why it's disabled
    if (!isAutosaveEnabled) {
      changelogEdit->setPlaceholderText(tr(
          "Changelog is only available when 'Automatically save the file' is enabled"));
      changelogLineLimitLabel->setText(tr("Enable automatic save to edit changelog"));
      changelogLineLimitLabel->setStyleSheet(
          "QLabel { color: gray; font-size: 10px; }");
    } else {
      changelogEdit->setPlaceholderText(tr(
          "Please write one entry per line. Example:\n* Added 64-bit support\n* Added "
          "raytracing...\n\n\nLeave it empty to not include a changelog"));
      updateChangelogLineMonitor();  // Restore normal monitoring
    }
  };

  // Connect the automaticSaveCheck signal to update changelog accessibility
  connect(automaticSaveCheck, &QCheckBox::stateChanged, updateChangelogAccessibility);

  // Initialize the changelog accessibility state
  updateChangelogAccessibility();

  // Buttons at the bottom
  QDialogButtonBox* buttons =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);

  // Style the buttons
  QPushButton* okButton     = buttons->button(QDialogButtonBox::Ok);
  QPushButton* cancelButton = buttons->button(QDialogButtonBox::Cancel);
  okButton->setDefault(true);
  okButton->setMinimumWidth(80);
  cancelButton->setMinimumWidth(80);

  mainLayout->addWidget(buttons);

  // Store mod info and other needed data for the async processing
  QString modDir     = modInfo->absolutePath();
  QString modName    = modInfo->name();
  QString domainName = modInfo->domainName();
  int nexusId        = modInfo->nexusId();
  int fileId         = modInfo->fileID();

  // Connect buttons to handle the result asynchronously
  connect(buttons, &QDialogButtonBox::accepted, dialog, [=]() {
    // Validate character limits before accepting
    if (filenameEdit->text().length() > 50) {
      QMessageBox::warning(dialog, tr("Filename Too Long"),
                           tr("Filename must be 50 characters or less."));
      return;
    }

    if (versionEdit->text().length() > 50) {
      QMessageBox::warning(dialog, tr("Version Too Long"),
                           tr("File version must be 50 characters or less."));
      return;
    }

    if (descriptionEdit->toPlainText().length() > 255) {
      QMessageBox::warning(dialog, tr("Description Too Long"),
                           tr("File description must be 255 characters or less."));
      return;
    }

    // Only validate changelog if automatic save is enabled and changelog has content
    if (automaticSaveCheck->isChecked()) {
      QString changelogText = changelogEdit->toPlainText();
      QStringList lines     = changelogText.split('\n');
      QList<int> longLines;

      for (int i = 0; i < lines.size(); ++i) {
        if (lines[i].length() > 50) {
          longLines.append(i + 1);  // +1 because line numbers start at 1 for users
        }
      }

      if (!longLines.isEmpty()) {
        // Convert line numbers to string list for display
        QStringList longLineStrings;
        for (int lineNum : longLines) {
          longLineStrings.append(QString::number(lineNum));
        }

        QString lineNumbers;
        if (longLineStrings.size() > 5) {
          lineNumbers = tr("lines %1, ... (and %2 more)")
                            .arg(longLineStrings.mid(0, 5).join(", "))
                            .arg(longLineStrings.size() - 5);
        } else {
          lineNumbers = tr("lines %1").arg(longLineStrings.join(", "));
        }

        QMessageBox::warning(
            dialog, tr("Changelog Lines Too Long"),
            tr("The following %1 exceed the 50 character limit:\n%2\n\nPlease shorten "
               "these lines before submitting.")
                .arg(longLines.size() == 1 ? tr("line") : tr("lines"))
                .arg(lineNumbers));
        return;
      }
    }

    if (rememberOptionsCheck->isChecked())
    {
      auto& _settings = const_cast<InterfaceSettings&>(m_core.settings().interface());
      _settings.setModUpdateToNXMLatestVersion(latestVersionCheck->isChecked());
      _settings.setModUpdateToNXMNewVersion(newVersionCheck->isChecked());
      _settings.setModUpdateToNXMRemovePreviousVersion(removePreviousVersionCheck->isChecked());
      _settings.setModUpdateToNXMRemoveDownloadWithManager(removeDownloadManagerCheck->isChecked());
      _settings.setModUpdateToNXMSetAsMainVortex(setFileAsMainVortexCheck->isChecked());
      _settings.setModUpdateToNXMInformDownloader(informRequirementsCheck->isChecked());
      _settings.setModUpdateToNXMAutoSaveFile(automaticSaveCheck->isChecked());
      _settings.setModUpdateToNXMUodateCurrentModVersionToMeta(updateModVersionMetaData->isChecked());
    }

    dialog->accept();
    handleUpdateDialogAccepted(
        modDir, modName, domainName, nexusId, fileId, filenameEdit->text(),
        versionEdit->text(), descriptionEdit->toPlainText(),
        changelogEdit->toPlainText(), latestVersionCheck->isChecked(),
        newVersionCheck->isChecked(), removePreviousVersionCheck->isChecked(),
        removeDownloadManagerCheck->isChecked(), setFileAsMainVortexCheck->isChecked(),
        informRequirementsCheck->isChecked(), automaticSaveCheck->isChecked(),
        updateModVersionMetaData->isChecked());
  });

  connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);

  // Show the dialog as modeless (non-blocking)
  dialog->show();
}

void ModListViewActions::handleUpdateDialogAccepted(
    const QString& modDir, const QString& modName, const QString& domainName,
    int nexusId, int fileId, const QString& filename, const QString& version,
    const QString& description, const QString& changelog, bool isLatestVersion,
    bool isExisting, bool removePreviousVersion, bool removeDownloadManager,
    bool isMainVortexFile, bool informDownloaders, bool autoSave,
    bool updateModVersionMetaData) const
{
  // Show a progress dialog to indicate work is starting
  QProgressDialog* progressDialog = new QProgressDialog(m_parent);
  progressDialog->setWindowTitle(tr("Updating Mod"));
  progressDialog->setLabelText(tr("Preparing mod archive..."));
  progressDialog->setRange(0, 0);  // Indeterminate progress
  progressDialog->setCancelButton(nullptr);
  progressDialog->show();

  // Create a worker object to handle the async process
  QObject* worker  = new QObject();
  QString gameName = "";
  if (domainName == "") {
    gameName = m_core.managedGame()->gameNexusName();
    if (gameName == "") {
      gameName = m_core.managedGame()->gameShortName();
      if (gameName == "")
        gameName = m_core.managedGame()->gameName();
    }
  } else {
    gameName = domainName;
  }

  // Step 1: Create archive asynchronously
  QString modArchive = QDir(modDir).filePath("mod_archive.7z");
  if (QFile::exists(modArchive)) {
    QFile::remove(modArchive);
  }

  progressDialog->setLabelText(tr("Creating archive..."));

  QProcess* zipProcess = new QProcess(worker);
  zipProcess->setWorkingDirectory(modDir);

  // Connect zip process signals
  connect(
      zipProcess, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), worker,
      [=](int exitCode, QProcess::ExitStatus exitStatus) {
        if (exitCode != 0 || exitStatus != QProcess::NormalExit) {
          QMetaObject::invokeMethod(
              qApp,
              [=]() {
                progressDialog->close();
                progressDialog->deleteLater();
                worker->deleteLater();
                reportError(tr("Zipping failed with error code: %1").arg(exitCode));
              },
              Qt::QueuedConnection);
          return;
        }

        // Step 2: Create JSON data (this is fast, can be done in main thread)
        QJsonObject jsonData;
        jsonData["gamename"]         = gameName;
        jsonData["modid"]            = nexusId;
        jsonData["fileid"]           = fileId;
        jsonData["filename"]         = filename.left(50);      // Enforce 50 char limit
        jsonData["fileversion"]      = version.left(50);       // Enforce 50 char limit
        jsonData["filedescription"]  = description.left(255);  // Enforce 255 char limit
        jsonData["fileabsolutepath"] = modArchive;
        jsonData["islatestversion"]  = isLatestVersion;
        jsonData["isexisting"]       = isExisting;
        jsonData["isremovepreviousversion"]       = removePreviousVersion;
        jsonData["isremovedownloadmanagerbutton"] = removeDownloadManager;
        jsonData["ismainvortexfile"]              = isMainVortexFile;
        jsonData["isinformdownloaders"]           = informDownloaders;
        jsonData["autosavefile"]                  = autoSave;
        jsonData["changelog"]                     = changelog;

        QJsonDocument jsonDoc(jsonData);

        // Get path to mo2 appdata
        QString baseDir = m_core.basePath();
        QDir dir(baseDir);
        baseDir = dir.absolutePath();
        QString appDataDir =
            QProcessEnvironment::systemEnvironment().value("LOCALAPPDATA");
        QString filePath = appDataDir + "/ModOrganizer/mod_update_cache.json";

        // Write JSON file
        QFile jsonFile(filePath);
        if (!jsonFile.open(QIODevice::WriteOnly)) {
          QMetaObject::invokeMethod(
              qApp,
              [=]() {
                progressDialog->close();
                progressDialog->deleteLater();
                worker->deleteLater();
                reportError(tr("Failed to write JSON file to: %1").arg(filePath));
              },
              Qt::QueuedConnection);
          return;
        }

        jsonFile.write(jsonDoc.toJson(QJsonDocument::Indented));
        jsonFile.close();
        log::info("JSON file written to: {}", filePath);

        // Step 3: Run Python script asynchronously
        progressDialog->setLabelText(tr("Running update script..."));

        QString targetDir = QCoreApplication::applicationDirPath() + "/mod_updater";
        QProcess* pythonProcess = new QProcess(worker);
        pythonProcess->setWorkingDirectory(targetDir);

        // Connect to capture Python's stdout and display as QInfo
        connect(pythonProcess, &QProcess::readyReadStandardOutput, worker, [=]() {
          QString output =
              QString::fromLocal8Bit(pythonProcess->readAllStandardOutput());
          log::debug("[Python]: {}", output);
        });

        // Also capture stderr for error messages
        connect(pythonProcess, &QProcess::readyReadStandardError, worker, [=]() {
          QString errorOutput =
              QString::fromLocal8Bit(pythonProcess->readAllStandardError());
          log::error("[Python Error]: {}", errorOutput);
        });

        connect(
            pythonProcess,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), worker,
            [=](int exitCode, QProcess::ExitStatus exitStatus) {
              QMetaObject::invokeMethod(
                  qApp,
                  [=]() {
                    progressDialog->close();
                    progressDialog->deleteLater();
                    worker->deleteLater();

                    if (exitCode == 0 && exitStatus == QProcess::NormalExit) {
                      log::info("Successfully updated mod: {}", modName);
                      QMessageBox::information(
                          m_parent, tr("Success"),
                          tr("Mod '%1' has been successfully updated.").arg(modName));
                      if (updateModVersionMetaData)
                      {
                        QString metaPath = modDir + "/meta.ini";
                        if (QFile::exists(metaPath)) {
                          QSettings metaFile(metaPath, QSettings::IniFormat);
                          if (metaFile.status() == QSettings::NoError) {
                            metaFile.setValue("newestVersion", version.left(50));
                            metaFile.setValue("version", version.left(50));
                            m_core.refresh();
                          }
                        } else {
                          log::error("Missing meta file at {}", metaPath);
                        }
                      }
                    } else {
                      QString errorMsg =
                          tr("Python script failed with exit code: %1").arg(exitCode);
                      if (exitStatus != QProcess::NormalExit) {
                        errorMsg = tr("Python script crashed");
                      }
                      QString stderrOutput =
                          QString::fromLocal8Bit(pythonProcess->readAllStandardError());
                      if (!stderrOutput.isEmpty()) {
                        errorMsg += "\n" + stderrOutput;
                      }
                      reportError(errorMsg);
                    }
                  },
                  Qt::QueuedConnection);
            });

        connect(pythonProcess, &QProcess::errorOccurred, worker,
                [=](QProcess::ProcessError error) {
                  QMetaObject::invokeMethod(
                      qApp,
                      [=]() {
                        progressDialog->close();
                        progressDialog->deleteLater();
                        worker->deleteLater();
                        reportError(tr("Failed to start Python process: %1")
                                        .arg(pythonProcess->errorString()));
                      },
                      Qt::QueuedConnection);
                });

        // Start the Python process
        pythonProcess->start("py", QStringList() << "update.py");
      });

  connect(
      zipProcess, &QProcess::errorOccurred, worker, [=](QProcess::ProcessError error) {
        QMetaObject::invokeMethod(
            qApp,
            [=]() {
              progressDialog->close();
              progressDialog->deleteLater();
              worker->deleteLater();
              reportError(
                  tr("Failed to start zip process: %1").arg(zipProcess->errorString()));
            },
            Qt::QueuedConnection);
      });

  // Start the zip process - use 7z with proper arguments
  QStringList zipArgs;
  zipArgs << "a" << "-x!meta.ini" << "mod_archive.7z" << "*";
  zipProcess->start("7z", zipArgs);
}

void ModListViewActions::renameMod(const QModelIndex& index) const
{
  try {
    m_view->edit(m_view->indexModelToView(index));
  } catch (const std::exception& e) {
    reportError(tr("failed to rename mod: %1").arg(e.what()));
  }
}

void ModListViewActions::removeMods(const QModelIndexList& indices) const
{
  const int max_items = 20;

  try {
    if (indices.size() > 1) {
      QString mods;
      QStringList modNames;

      int i = 0;
      for (auto& idx : indices) {
        QString name = idx.data().toString();
        if (!ModInfo::getByIndex(idx.data(ModList::IndexRole).toInt())->isRegular()) {
          continue;
        }

        // adds an item for the mod name until `i` reaches `max_items`, which
        // adds one "..." item; subsequent mods are not shown on the list but
        // are still added to `modNames` below so they can be removed correctly

        if (i < max_items) {
          mods += "<li>" + name + "</li>";
        } else if (i == max_items) {
          mods += "<li>...</li>";
        }

        modNames.append(
            ModInfo::getByIndex(idx.data(ModList::IndexRole).toInt())->name());
        ++i;
      }
      const auto r =
          MOBase::TaskDialog(nullptr, tr("Delete multiple mods"))
              .main(tr("Remove the following mods?<br><ul>%1</ul>").arg(mods))
              .icon(QMessageBox::Question)
              .button({tr("Move to the Recycle Bin"), QMessageBox::Yes})
              .button({tr("Delete permanently"), QMessageBox::Ok})
              .button({tr("Cancel"), QMessageBox::Cancel})
              .remember("rememberMultipleModsRowDeletion")
              .exec();

      switch (r) {
      case QMessageBox::Yes:
        DownloadManager::startDisableDirWatcher();
        for (QString name : modNames) {
          m_core.modList()->removeRowForce(ModInfo::getIndex(name), QModelIndex());
        }
        DownloadManager::endDisableDirWatcher();
        break;
      case QMessageBox::Ok:
        DownloadManager::startDisableDirWatcher();
        for (QString name : modNames) {
          m_core.modList()->removeRowForce(ModInfo::getIndex(name), QModelIndex(),
                                           true);
        }
        DownloadManager::endDisableDirWatcher();
        break;
      }
    } else if (!indices.isEmpty())
      m_core.modList()->removeRow(indices[0].data(ModList::IndexRole).toInt(),
                                  QModelIndex());

    m_view->updateModCount();
    m_pluginView->updatePluginCount();
  } catch (const std::exception& e) {
    reportError(tr("failed to remove mod: %1").arg(e.what()));
  }
}

void ModListViewActions::ignoreMissingData(const QModelIndexList& indices) const
{
  for (auto& idx : indices) {
    int row_idx       = idx.data(ModList::IndexRole).toInt();
    ModInfo::Ptr info = ModInfo::getByIndex(row_idx);
    info->markValidated(true);
    m_core.modList()->notifyChange(row_idx);
  }
}

void ModListViewActions::setIgnoreUpdate(const QModelIndexList& indices,
                                         bool ignore) const
{
  for (auto& idx : indices) {
    int modIdx        = idx.data(ModList::IndexRole).toInt();
    ModInfo::Ptr info = ModInfo::getByIndex(modIdx);
    info->ignoreUpdate(ignore);
    m_core.modList()->notifyChange(modIdx);
  }
}

void ModListViewActions::changeVersioningScheme(const QModelIndex& index) const
{
  if (QMessageBox::question(
          m_parent, tr("Continue?"),
          tr("The versioning scheme decides which version is considered newer than "
             "another.\n"
             "This function will guess the versioning scheme under the assumption that "
             "the installed version is outdated."),
          QMessageBox::Yes | QMessageBox::Cancel) == QMessageBox::Yes) {

    ModInfo::Ptr info = ModInfo::getByIndex(index.data(ModList::IndexRole).toInt());

    bool success = false;

    static VersionInfo::VersionScheme schemes[] = {
        VersionInfo::SCHEME_REGULAR, VersionInfo::SCHEME_DECIMALMARK,
        VersionInfo::SCHEME_NUMBERSANDLETTERS};

    for (int i = 0;
         i < sizeof(schemes) / sizeof(VersionInfo::VersionScheme) && !success; ++i) {
      VersionInfo verOld(info->version().canonicalString(), schemes[i]);
      VersionInfo verNew(info->newestVersion().canonicalString(), schemes[i]);
      if (verOld < verNew) {
        info->setVersion(verOld);
        info->setNewestVersion(verNew);
        success = true;
      }
    }
    if (!success) {
      QMessageBox::information(
          m_parent, tr("Sorry"),
          tr("I don't know a versioning scheme where %1 is newer than %2.")
              .arg(info->newestVersion().canonicalString())
              .arg(info->version().canonicalString()),
          QMessageBox::Ok);
    }
  }
}

void ModListViewActions::markConverted(const QModelIndexList& indices) const
{
  for (auto& idx : indices) {
    int modIdx        = idx.data(ModList::IndexRole).toInt();
    ModInfo::Ptr info = ModInfo::getByIndex(modIdx);
    info->markConverted(true);
    m_core.modList()->notifyChange(modIdx);
  }
}

void ModListViewActions::visitOnNexus(const QModelIndexList& indices) const
{
  if (indices.size() > 10) {
    if (!askOpenLinksConfirmation(indices.size(), tr("Nexus Links"))) {
      return;
    }
  }

  for (auto& idx : indices) {
    ModInfo::Ptr info = ModInfo::getByIndex(idx.data(ModList::IndexRole).toInt());
    int modID         = info->nexusId();
    QString gameName  = info->gameName();
    if (modID > 0) {
      shell::Open(QUrl(NexusInterface::instance().getModURL(modID, gameName)));
    } else {
      log::error("mod '{}' has no nexus id", info->name());
    }
  }
}

void ModListViewActions::visitWebPage(const QModelIndexList& indices) const
{
  if (indices.size() > 10) {
    if (!askOpenLinksConfirmation(indices.size(), tr("Web Pages"))) {
      return;
    }
  }

  for (auto& idx : indices) {
    ModInfo::Ptr info = ModInfo::getByIndex(idx.data(ModList::IndexRole).toInt());

    const auto url = info->parseCustomURL();
    if (url.isValid()) {
      shell::Open(url);
    }
  }
}

void ModListViewActions::visitNexusOrWebPage(const QModelIndexList& indices) const
{
  if (indices.size() > 10) {
    if (!askOpenLinksConfirmation(indices.size(), tr("Web Pages"))) {
      return;
    }
  }

  for (auto& idx : indices) {
    ModInfo::Ptr info = ModInfo::getByIndex(idx.data(ModList::IndexRole).toInt());
    if (!info) {
      log::error("mod {} not found", idx.data(ModList::IndexRole).toInt());
      continue;
    }

    int modID        = info->nexusId();
    QString gameName = info->gameName();
    const auto url   = info->parseCustomURL();

    if (modID > 0) {
      shell::Open(QUrl(NexusInterface::instance().getModURL(modID, gameName)));
    } else if (url.isValid()) {
      shell::Open(url);
    } else {
      log::error("mod '{}' has no valid link", info->name());
    }
  }
}

void ModListViewActions::visitUploaderProfile(const QModelIndexList& indices) const
{
  if (indices.size() > 10) {
    if (!askOpenLinksConfirmation(indices.size(), tr("Uploader Profiles"))) {
      return;
    }
  }

  for (auto& idx : indices) {
    ModInfo::Ptr info      = ModInfo::getByIndex(idx.data(ModList::IndexRole).toInt());
    const auto uploaderUrl = info->uploaderUrl();

    if (!uploaderUrl.isEmpty()) {
      shell::Open(QUrl(uploaderUrl));
    } else {
      log::error("mod '{}' has no uploader url", info->name());
    }
  }
}

bool ModListViewActions::askOpenLinksConfirmation(std::size_t numberOfLinks,
                                                  const QString& nameOfLinks) const
{
  return QMessageBox::question(m_parent, tr("Opening %1").arg(nameOfLinks),
                               tr("You are trying to open %1 %2. Are you sure "
                                  "you want to do this?")
                                   .arg(numberOfLinks)
                                   .arg(nameOfLinks),
                               QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes;
}

void ModListViewActions::reinstallMod(const QModelIndex& index) const
{
  ModInfo::Ptr modInfo = ModInfo::getByIndex(index.data(ModList::IndexRole).toInt());
  QString installationFile = modInfo->installationFile();
  if (installationFile.length() != 0) {
    QString fullInstallationFile;
    QFileInfo fileInfo(installationFile);
    if (fileInfo.isAbsolute()) {
      if (fileInfo.exists()) {
        fullInstallationFile = installationFile;
      } else {
        fullInstallationFile =
            m_core.downloadManager()->getOutputDirectory() + "/" + fileInfo.fileName();
      }
    } else {
      fullInstallationFile =
          m_core.downloadManager()->getOutputDirectory() + "/" + installationFile;
    }
    if (QFile::exists(fullInstallationFile)) {
      m_core.installMod(fullInstallationFile, -1, true, modInfo, modInfo->name());
    } else {
      QMessageBox::information(m_parent, tr("Failed"),
                               tr("Installation file no longer exists"));
    }
  } else {
    QMessageBox::information(
        m_parent, tr("Failed"),
        tr("Mods installed with old versions of MO can't be reinstalled in this way."));
  }
}

void ModListViewActions::createBackup(const QModelIndex& index) const
{
  ModInfo::Ptr modInfo = ModInfo::getByIndex(index.data(ModList::IndexRole).toInt());
  QString backupDirectory =
      m_core.installationManager()->generateBackupName(modInfo->absolutePath());
  if (!copyDir(modInfo->absolutePath(), backupDirectory, false)) {
    QMessageBox::information(m_parent, tr("Failed"), tr("Failed to create backup."));
  }
  m_core.refresh();
  m_view->updateModCount();
}

void ModListViewActions::restoreHiddenFiles(const QModelIndexList& indices) const
{
  const int max_items = 20;

  QFlags<FileRenamer::RenameFlags> flags = FileRenamer::UNHIDE;
  flags |= FileRenamer::MULTIPLE;

  FileRenamer renamer(m_parent, flags);

  FileRenamer::RenameResults result = FileRenamer::RESULT_OK;

  // multi selection
  if (indices.size() > 1) {

    QStringList modNames;
    for (auto& idx : indices) {

      ModInfo::Ptr modInfo = ModInfo::getByIndex(idx.data(ModList::IndexRole).toInt());
      const auto flags     = modInfo->getFlags();

      if (!modInfo->isRegular() ||
          std::find(flags.begin(), flags.end(), ModInfo::FLAG_HIDDEN_FILES) ==
              flags.end()) {
        continue;
      }

      modNames.append(idx.data(Qt::DisplayRole).toString());
    }

    QString mods = "<li>" + modNames.mid(0, max_items).join("</li><li>") + "</li>";
    if (modNames.size() > max_items) {
      mods += "<li>...</li>";
    }

    if (QMessageBox::question(
            m_parent, tr("Confirm"),
            tr("Restore all hidden files in the following mods?<br><ul>%1</ul>")
                .arg(mods),
            QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes) {

      for (auto& idx : indices) {

        ModInfo::Ptr modInfo =
            ModInfo::getByIndex(idx.data(ModList::IndexRole).toInt());

        const auto flags = modInfo->getFlags();
        if (std::find(flags.begin(), flags.end(), ModInfo::FLAG_HIDDEN_FILES) !=
            flags.end()) {
          const QString modDir = modInfo->absolutePath();

          auto partialResult = restoreHiddenFilesRecursive(renamer, modDir);

          if (partialResult == FileRenamer::RESULT_CANCEL) {
            result = FileRenamer::RESULT_CANCEL;
            break;
          }
          emit originModified((m_core.directoryStructure()->getOriginByName(
                                   ToWString(modInfo->internalName())))
                                  .getID());
        }
      }
    }
  } else if (!indices.isEmpty()) {
    // single selection
    ModInfo::Ptr modInfo =
        ModInfo::getByIndex(indices[0].data(ModList::IndexRole).toInt());
    const QString modDir = modInfo->absolutePath();

    if (QMessageBox::question(
            m_parent, tr("Are you sure?"),
            tr("About to restore all hidden files in:\n") + modInfo->name(),
            QMessageBox::Ok | QMessageBox::Cancel) == QMessageBox::Ok) {

      result = restoreHiddenFilesRecursive(renamer, modDir);

      emit originModified((m_core.directoryStructure()->getOriginByName(
                               ToWString(modInfo->internalName())))
                              .getID());
    }
  }

  if (result == FileRenamer::RESULT_CANCEL) {
    log::debug("Restoring hidden files operation cancelled");
  } else {
    log::debug("Finished restoring hidden files");
  }
}

void ModListViewActions::setTracked(const QModelIndexList& indices, bool tracked) const
{
  m_core.loggedInAction(m_parent, [=] {
    for (auto& idx : indices) {
      ModInfo::getByIndex(idx.data(ModList::IndexRole).toInt())->track(tracked);
    }
  });
}

void ModListViewActions::setEndorsed(const QModelIndexList& indices,
                                     bool endorsed) const
{
  m_core.loggedInAction(m_parent, [=] {
    if (indices.size() > 1) {
      MessageDialog::showMessage(
          tr("Endorsing multiple mods will take a while. Please wait..."), m_parent);
    }

    for (auto& idx : indices) {
      ModInfo::getByIndex(idx.data(ModList::IndexRole).toInt())->endorse(endorsed);
    }
  });
}

void ModListViewActions::willNotEndorsed(const QModelIndexList& indices) const
{
  for (auto& idx : indices) {
    ModInfo::getByIndex(idx.data(ModList::IndexRole).toInt())->setNeverEndorse();
  }
}

void ModListViewActions::remapCategory(const QModelIndexList& indices) const
{
  for (auto& idx : indices) {
    ModInfo::Ptr modInfo = ModInfo::getByIndex(idx.data(ModList::IndexRole).toInt());
    if (modInfo->isSeparator())
      continue;

    int categoryID = modInfo->getNexusCategory();
    if (!categoryID) {
      QSettings downloadMeta(m_core.downloadsPath() + "/" +
                                 modInfo->installationFile() + ".meta",
                             QSettings::IniFormat);
      if (downloadMeta.contains("category")) {
        categoryID = downloadMeta.value("category", 0).toInt();
      }
    }
    unsigned int categoryIndex = CategoryFactory::instance().resolveNexusID(categoryID);
    if (categoryIndex != 0)
      modInfo->setPrimaryCategory(
          CategoryFactory::instance().getCategoryID(categoryIndex));
  }
}

void ModListViewActions::setColor(const QModelIndexList& indices,
                                  const QModelIndex& refIndex) const
{
  auto& settings       = m_core.settings();
  ModInfo::Ptr modInfo = ModInfo::getByIndex(refIndex.data(ModList::IndexRole).toInt());

  QColorDialog dialog(m_parent);
  dialog.setOption(QColorDialog::ShowAlphaChannel);

  QColor currentColor = modInfo->color();
  if (currentColor.isValid()) {
    dialog.setCurrentColor(currentColor);
  } else if (auto c = settings.colors().previousSeparatorColor()) {
    dialog.setCurrentColor(*c);
  }

  if (!dialog.exec())
    return;

  currentColor = dialog.currentColor();
  if (!currentColor.isValid())
    return;

  settings.colors().setPreviousSeparatorColor(currentColor);

  for (auto& idx : indices) {
    ModInfo::Ptr info = ModInfo::getByIndex(idx.data(ModList::IndexRole).toInt());
    info->setColor(currentColor);
  }
}

void ModListViewActions::resetColor(const QModelIndexList& indices) const
{
  for (auto& idx : indices) {
    ModInfo::Ptr info = ModInfo::getByIndex(idx.data(ModList::IndexRole).toInt());
    info->setColor(QColor());
  }
  m_core.settings().colors().removePreviousSeparatorColor();
}

void ModListViewActions::setCategories(
    ModInfo::Ptr mod, const std::vector<std::pair<int, bool>>& categories) const
{
  for (auto& [id, enabled] : categories) {
    mod->setCategory(id, enabled);
  }
}

void ModListViewActions::setCategoriesIf(
    ModInfo::Ptr mod, ModInfo::Ptr ref,
    const std::vector<std::pair<int, bool>>& categories) const
{
  for (auto& [id, enabled] : categories) {
    if (ref->categorySet(id) != enabled) {
      mod->setCategory(id, enabled);
    }
  }
}

void ModListViewActions::setCategories(
    const QModelIndexList& selected, const QModelIndex& ref,
    const std::vector<std::pair<int, bool>>& categories) const
{
  ModInfo::Ptr refMod = ModInfo::getByIndex(ref.data(ModList::IndexRole).toInt());
  if (selected.size() > 1) {
    for (auto& idx : selected) {
      if (idx.row() != ref.row()) {
        setCategoriesIf(ModInfo::getByIndex(idx.data(ModList::IndexRole).toInt()),
                        refMod, categories);
      }
    }
    setCategories(refMod, categories);
  } else if (!selected.isEmpty()) {
    // for single mod selections, just do a replace
    setCategories(refMod, categories);
  }

  for (auto& idx : selected) {
    m_core.modList()->notifyChange(idx.data(ModList::IndexRole).toInt());
  }

  // reset the selection manually - still needed
  auto viewIndices = m_view->indexModelToView(selected);
  for (auto& idx : viewIndices) {
    m_view->selectionModel()->select(idx, QItemSelectionModel::Select |
                                              QItemSelectionModel::Rows);
  }
}

void ModListViewActions::setPrimaryCategory(const QModelIndexList& selected,
                                            int category, bool force)
{
  for (auto& idx : selected) {
    ModInfo::Ptr info = ModInfo::getByIndex(idx.data(ModList::IndexRole).toInt());
    if (force || info->categorySet(category)) {
      info->setCategory(category, true);
      info->setPrimaryCategory(category);
    }
  }

  // reset the selection manually - still needed
  auto viewIndices = m_view->indexModelToView(selected);
  for (auto& idx : viewIndices) {
    m_view->selectionModel()->select(idx, QItemSelectionModel::Select |
                                              QItemSelectionModel::Rows);
  }
}

void ModListViewActions::openExplorer(const QModelIndexList& index) const
{
  for (auto& idx : index) {
    ModInfo::Ptr info = ModInfo::getByIndex(idx.data(ModList::IndexRole).toInt());
    if (!info->isForeign()) {
      shell::Explore(info->absolutePath());
    }
  }
}

void ModListViewActions::restoreBackup(const QModelIndex& index) const
{
  QRegularExpression backupRegEx("(.*)_backup[0-9]*$");
  ModInfo::Ptr modInfo = ModInfo::getByIndex(index.data(ModList::IndexRole).toInt());
  auto match           = backupRegEx.match(modInfo->name());
  if (match.hasMatch()) {
    QString regName = match.captured(1);
    QDir modDir(QDir::fromNativeSeparators(m_core.settings().paths().mods()));
    if (!modDir.exists(regName) ||
        (QMessageBox::question(
             m_parent, tr("Overwrite?"),
             tr("This will replace the existing mod \"%1\". Continue?").arg(regName),
             QMessageBox::Yes | QMessageBox::No) == QMessageBox::Yes)) {
      if (modDir.exists(regName) &&
          !shellDelete(QStringList(modDir.absoluteFilePath(regName)))) {
        reportError(tr("failed to remove mod \"%1\"").arg(regName));
      } else {
        QString destinationPath =
            QDir::fromNativeSeparators(m_core.settings().paths().mods()) + "/" +
            regName;
        if (!modDir.rename(modInfo->absolutePath(), destinationPath)) {
          reportError(tr("failed to rename \"%1\" to \"%2\"")
                          .arg(modInfo->absolutePath())
                          .arg(destinationPath));
        }
        m_core.refresh();
        m_view->updateModCount();
      }
    }
  }
}

void ModListViewActions::moveOverwriteContentsTo(const QString& absolutePath) const
{
  ModInfo::Ptr overwriteInfo = ModInfo::getOverwrite();
  bool successful            = false;
  if (m_core.managedGame()->getModMappings().count() > 1 ||
      m_core.managedGame()->getModMappings().keys().first() != "") {
    QDirIterator iter(overwriteInfo->absolutePath(),
                      QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot);
    while (iter.hasNext()) {
      auto entry = iter.nextFileInfo();
      if (entry.isDir() && m_core.managedGame()->getModMappings().keys().contains(
                               entry.fileName(), Qt::CaseInsensitive)) {
        successful =
            shellCopy((QDir::toNativeSeparators(entry.absolutePath())),
                      (QDir::toNativeSeparators(absolutePath)), false, m_parent);
        QDirIterator subDirIter(entry.absoluteFilePath(),
                                QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot);
        while (subDirIter.hasNext()) {
          auto subDirEntry = subDirIter.nextFileInfo();
          if (subDirEntry.isDir()) {
            QDir(subDirEntry.absoluteFilePath()).removeRecursively();
          } else {
            QFile(subDirEntry.absoluteFilePath()).remove();
          }
        }
      } else {
        successful =
            shellMove((QDir::toNativeSeparators(iter.filePath())),
                      (QDir::toNativeSeparators(absolutePath)), false, m_parent);
      }
      if (!successful)
        break;
    }

  } else {
    successful =
        shellMove((QDir::toNativeSeparators(overwriteInfo->absolutePath()) + "\\*"),
                  (QDir::toNativeSeparators(absolutePath)), false, m_parent);
  }

  if (successful) {
    MessageDialog::showMessage(tr("Move successful."), m_parent);
  } else {
    const auto e = GetLastError();
    log::error("Move operation failed: {}", formatSystemMessage(e));
  }

  m_core.refresh();
}

void ModListViewActions::createModFromOverwrite() const
{
  GuessedValue<QString> name;
  name.setFilter(&fixDirectoryName);

  while (name->isEmpty()) {
    bool ok;
    name.update(
        QInputDialog::getText(
            m_parent, tr("Create Mod..."),
            tr("This will move all files from overwrite into a new, regular mod.\n"
               "Please enter a name:"),
            QLineEdit::Normal, "", &ok),
        GUESS_USER);
    if (!ok) {
      return;
    }
  }

  if (m_core.modList()->getMod(name) != nullptr) {
    reportError(tr("A mod with this name already exists"));
    return;
  }

  const IModInterface* newMod = m_core.createMod(name);
  if (newMod == nullptr) {
    return;
  }

  moveOverwriteContentsTo(newMod->absolutePath());
}

void ModListViewActions::moveOverwriteContentToExistingMod() const
{
  QStringList mods;
  auto indexesByPriority = m_core.currentProfile()->getAllIndexesByPriority();
  for (auto& iter : indexesByPriority) {
    if ((iter.second != UINT_MAX)) {
      ModInfo::Ptr modInfo = ModInfo::getByIndex(iter.second);
      if (!modInfo->isSeparator() && !modInfo->isForeign() && !modInfo->isOverwrite()) {
        mods << modInfo->name();
      }
    }
  }

  ListDialog dialog(m_parent);
  dialog.setWindowTitle("Select a mod...");
  dialog.setChoices(mods);

  if (dialog.exec() == QDialog::Accepted) {
    QString result = dialog.getChoice();
    if (!result.isEmpty()) {

      QString modAbsolutePath;

      for (const auto& mod : m_core.modList()->allModsByProfilePriority()) {
        if (result.compare(mod) == 0) {
          ModInfo::Ptr modInfo = ModInfo::getByIndex(ModInfo::getIndex(mod));
          modAbsolutePath      = modInfo->absolutePath();
          break;
        }
      }

      if (modAbsolutePath.isNull()) {
        log::warn("Mod {} has not been found, for some reason", result);
        return;
      }

      moveOverwriteContentsTo(modAbsolutePath);
    }
  }
}

void ModListViewActions::clearOverwrite() const
{
  ModInfo::Ptr modInfo = ModInfo::getOverwrite();
  if (modInfo) {
    QDir overwriteDir(modInfo->absolutePath());
    if (QMessageBox::question(
            m_parent, tr("Are you sure?"),
            tr("About to recursively delete:\n") + overwriteDir.absolutePath(),
            QMessageBox::Ok | QMessageBox::Cancel) == QMessageBox::Ok) {
      QStringList delList;
      for (auto f : overwriteDir.entryInfoList(QDir::AllDirs | QDir::Files |
                                               QDir::NoDotAndDotDot)) {
        if (f.isDir() && m_core.managedGame()->getModMappings().keys().contains(
                             f.fileName(), Qt::CaseInsensitive)) {
          for (auto sf :
               QDir(f.absoluteFilePath())
                   .entryInfoList(QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot)) {
            delList.push_back(sf.absoluteFilePath());
          }
        } else {
          delList.push_back(f.absoluteFilePath());
        }
      }
      if (shellDelete(delList, true)) {
        emit overwriteCleared();
        m_core.refresh();
      } else {
        const auto e = GetLastError();
        log::error("Delete operation failed: {}", formatSystemMessage(e));
      }
    }
  }
}
