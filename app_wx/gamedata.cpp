#include "gamedata.hpp"

#include "wx/boxes.hpp"
#include "notifier.hpp"
#include "misc/dirfinder.hpp"
#include "importer_gui.hpp"
#include <boost/filesystem.hpp>
#include <spread/misc/readjson.hpp>
#include "launcher/run.hpp"

namespace bf = boost::filesystem;
using namespace TigData;
using namespace wxTigApp;
using namespace TigLib;

//#define PRINT_DEBUG

#ifdef PRINT_DEBUG
#include <iostream>
#define PRINT(a) std::cout << a << "\n"
#else
#define PRINT(a)
#endif

void GameNews::reload()
{
  news.reload();
  items.resize(news.size());

  for(int i=0; i<news.size(); i++)
    {
      wxGameNewsItem &out = items[i];
      const TigLib::NewsItem &in = news.get(i);

      out.read = in.isRead;
      out.dateNum = in.date;
      out.subject = strToWx(in.subject);
      out.body = strToWx(in.body);

      char buf[50];
      strftime(buf,50, "%Y-%m-%d", gmtime(&in.date));
      out.date = wxString(buf, wxConvUTF8);
    }
}

void GameNews::markAsRead(int i)
{
  news.markAsRead(i);
  items[i].read = true;
}

void GameNews::markAllAsRead()
{
  news.markAllAsRead();
  for(int i=0; i<items.size(); i++)
    items[i].read = true;
}

struct FreeDemoPick : GamePicker
{
  bool free;
  FreeDemoPick(bool _free) : free(_free) {}

  bool include(const LiveInfo *inf)
  { return inf->ent->isDemo() != free; }
};

struct InstalledPick : GamePicker
{
  bool include(const LiveInfo *inf) { return !inf->isUninstalled(); }
};

static FreeDemoPick freePick(true), demoPick(false);
static InstalledPick instPick;

void wxTigApp::GameData::updateReady()
{
  PRINT("GameData::updateReady()");
  PRINT("  repo.hasNewData():  " << repo.hasNewData());
  PRINT("  hasNewUpdate:       " << updater.hasNewUpdate);
  PRINT("  newExePath:         " << updater.newExePath);
  PRINT("  newVersion:         " << updater.newVersion);

  if(!listener) return;

  // Load the updated news file and refresh the display
  listener->refreshNews();

  // Check if there was actually any new data in the repo
  if(!repo.hasNewData())
    {
      PRINT("No new data available");

      // Just update the stats
      repo.loadStats();
      updateDisplayStatus();
      return;
    }

  /* If we got here, a full data update is necessary.

     However, we might have gotten a program update as well, which
     trumps data updates, and instead issues a request to restart the
     entire program. (Restarting reloads the data too, of course.)
   */

  if(updater.hasNewUpdate)
    {
      /* If a new version is available, notify the user so they can
         restart. No further action is needed, any restart at this
         point (even a crash) will work.

         When the user presses the displayed button, notifyButton() is
         invoked, and the program is restarted.

         If a new version is available, we do NOT reload the data.
         This is because the new data may be packaged in a new format
         that the current version doesn't know how to handle.

         This is by design, so that we can update the client and data
         simultaneously, without worrying about cross-version
         compatibility.
      */
      PRINT("New version available: " << updater.newVersion << ", notifying user.");

      listener->displayNotification("Tiggit has been updated to version " + updater.newVersion, "Restart now", 2);
    }

  else
    {
      /* Data update but no program update. Don't ask the user, just
         do it immediately.
      */
      PRINT("Pure data update. Reloading data.");
      try { loadData(); }
      catch(std::exception &e)
        {
          Boxes::error(e.what());
        }
      catch(...)
        {
          Boxes::error("Unknown error while loading data");
        }
    }
}

void wxTigApp::GameData::notifyButton(int id)
{
  PRINT("notifyButton(" << id << ")");

  assert(id == 2);
  if(updater.launchNew())
    {
      PRINT("Launched another executable. Exiting now.");
      assert(frame);
      frame->Close();
    }
  else
    {
      PRINT("No process launched. Continuing this one instead.");
    }
}

bool wxTigApp::GameData::isActive() { return notify.hasJobs(); }

bool wxTigApp::GameData::moveRepo(const std::string &newPath)
{
  PRINT("GameData::moveRepo(" << newPath << ")");

  // First, check if the new path is usable. If the path is the same
  // as the old path, exit with success.
  if(!Misc::DirFinder::isWritable(newPath))
    return false;

  // NOTE: All points below this return 'true', even on error. The
  // 'false' return value is ONLY used to signal a non-writable path.

  // We do NOT allow running this function if downloads are currently
  // in progress (TODO: later we will simply stash the jobs, and
  // resume them on restart.)
  if(notify.hasJobs())
    {
      Boxes::error("Cannot change directories while downloads are in progress");
      return true;
    }

  try
    {
      Spread::SpreadLib *spread = &repo.getSpread();

      // Import main data (games, screenshots and config files.) The
      // last 'false' parameter means 'do not delete source files'.
      if(!ImportGui::importRepoGui(repo.getPath(), newPath, spread, false))
        return true;

      bf::path oldP = repo.getPath(), newP = newPath;

      // Next, copy executables and spread files.
      if(!ImportGui::copyFilesGui((oldP/"run").string(), (newP/"run").string(),
                                  spread, "Copying executables"))
        return true;
      if(!ImportGui::copyFilesGui((oldP/"spread/channels").string(),
                                  (newP/"spread/channels").string(),
                                  spread, "Copying Tiggit data"))
        return true;

      bf::copy_file(oldP/"spread/cache.conf", newP/"spread/cache.conf");

      /* Create a cleanup file in the new repo. This will ask the user
         if they want to delete the old repository after we've
         restarted.
       */
      ReadJson::writeJson((newP/"cleanup.json").string(), oldP.string(), true);

      // Finally, switch the globally stored path string over to the
      // new location. This makes this the "official" repository from
      // now on.
      repo.setStoredPath(newPath);

      // Notify the user that we are restarting from the new location
      Boxes::say("Tiggit will now restart for changes to take effect");

      // Launch the new exe
      newP = newP / "run" / "1";
      try { Launcher::run((newP/"tiggit.exe").string(), newP.string()); }
      catch(std::exception &e) { Boxes::error(e.what()); }
      frame->Close();
    }
  catch(std::exception &e)
    {
      Boxes::error(e.what());
    }
  catch(...)
    {
      Boxes::error("An unknown error occured");
    }

  return true;
}

void wxTigApp::GameData::killData()
{
  PRINT("GameData::killData()");

  const InfoLookup &lst = repo.getList();
  InfoLookup::const_iterator it;
  for(it=lst.begin(); it!=lst.end(); it++)
    {
      LiveInfo *li = it->second;
      GameInf *gi = (GameInf*)(li->extra);
      if(gi) delete gi;
      li->extra = NULL;
    }
}

void wxTigApp::GameData::loadData()
{
  PRINT("loadData()");

  using namespace std;

  // First, kill any existing data structures
  killData();

  // Then, load the data
  PRINT("Loading data now");
  repo.loadData();

  PRINT("Attaching GameInf structures");

  // Create GameInf structs attached to all the LiveInfo structs
  const InfoLookup &lst = repo.getList();
  InfoLookup::const_iterator it;
  for(it=lst.begin(); it!=lst.end(); it++)
    {
      LiveInfo *li = it->second;
      assert(li->extra == NULL);
      li->extra = new GameInf(li, &config);
    }

  /* Transfer existing install jobs, if any, over to the new LiveInfo
     structs. Installs-in-progress will thus "survive" a data reload
     uninterrupted.

     The StatusNotifier (notify) does all the grunt work of looking up
     the structures based on idname.
  */
  notify.reassignJobs();

  /* Propagate update notifications down the list hierarchy. This has
     to be called AFTER the GameInf structures are set up, otherwise
     the wx display classes will get update notifications before there
     is any data to update from.
   */
  PRINT("Calling repo.doneLoading()");
  repo.doneLoading();

  // Notify every list that the data has been reloaded
  PRINT("Notifying all lists");
  notifyReloaded();
}

wxTigApp::GameData::GameData(Repo &rep)
  : config(rep.getPath("wxtiggit.conf")), news(&rep),
    repo(rep), updater(rep)
{
  repo.getSpread().getJobManager()->setLogger(rep.getPath("threads.log"));

  latest = new GameList(rep.baseList(), NULL);
  freeware = new GameList(rep.baseList(), &freePick);
  demos = new GameList(rep.baseList(), &demoPick);
  installed = new GameList(rep.baseList(), &instPick);
}

wxTigApp::GameData::~GameData()
{
  delete latest;
  delete freeware;
  delete demos;
  delete installed;

  killData();
}
