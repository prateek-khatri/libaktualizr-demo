#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <openssl/ssl.h>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/signals2.hpp>

#include "config/config.h"
#include "logging/logging.h"
#include "primary/aktualizr.h"
#include "primary/results.h"
#include "utilities/utils.h"

namespace bpo = boost::program_options;

std::string campaign_id_selected;
Json::Value custom_manifest;

void check_info_options(const bpo::options_description &description, const bpo::variables_map &vm) {
  if (vm.count("help") != 0) {
    std::cout << description << '\n';
    std::cout << "Available commands: Shutdown, SendDeviceData, CheckUpdates, Download, Install, CampaignCheck\n";
    exit(EXIT_SUCCESS);
  }
}

bpo::variables_map parse_options(int argc, char *argv[]) {
  bpo::options_description description("HMI stub interface for libaktualizr");
  description.add_options()
      ("help,h", "print usage")
      ("config,c", bpo::value<std::vector<boost::filesystem::path> >()->composing(), "configuration file or directory")
      ("loglevel", bpo::value<int>(), "set log level 0-5 (trace, debug, info, warning, error, fatal)");

  bpo::variables_map vm;
  std::vector<std::string> unregistered_options;
  try {
    bpo::basic_parsed_options<char> parsed_options =
        bpo::command_line_parser(argc, argv).options(description).allow_unregistered().run();
    bpo::store(parsed_options, vm);
    check_info_options(description, vm);
    bpo::notify(vm);
    unregistered_options = bpo::collect_unrecognized(parsed_options.options, bpo::include_positional);
    if (vm.count("help") == 0 && !unregistered_options.empty()) {
      std::cout << description << "\n";
      exit(EXIT_FAILURE);
    }
  } catch (const bpo::required_option &ex) {
    // print the error and append the default commandline option description
    std::cout << ex.what() << std::endl << description;
    exit(EXIT_FAILURE);
  } catch (const bpo::error &ex) {
    check_info_options(description, vm);

    // log boost error
    LOG_ERROR << "boost command line option error: " << ex.what();

    // print the error message to the standard output too, as the user provided
    // a non-supported commandline option
    std::cout << ex.what() << '\n';

    // set the returnValue, thereby ctest will recognize
    // that something went wrong
    exit(EXIT_FAILURE);
  }

  return vm;
}

static std::vector<Uptane::Target> current_updates;

void process_event(const std::shared_ptr<event::BaseEvent> &event) {
  static std::map<std::string, unsigned int> progress;

  if (event->isTypeOf(event::DownloadProgressReport::TypeName)) {
    const auto download_progress = dynamic_cast<event::DownloadProgressReport *>(event.get());
    if (progress.find(download_progress->target.sha256Hash()) == progress.end()) {
      progress[download_progress->target.sha256Hash()] = 0;
    }
    const unsigned int prev_progress = progress[download_progress->target.sha256Hash()];
    const unsigned int new_progress = download_progress->progress;
    if (new_progress > prev_progress) {
      progress[download_progress->target.sha256Hash()] = new_progress;
      std::cout << "Download progress for file " << download_progress->target.filename() << ": " << new_progress
                << "%\n";
    }
  } else if (event->variant == "DownloadTargetComplete") {
    const auto download_complete = dynamic_cast<event::DownloadTargetComplete *>(event.get());
    std::cout << "Download complete for file " << download_complete->update.filename() << ": "
              << (download_complete->success ? "success" : "failure") << "\n";
    progress.erase(download_complete->update.sha256Hash());
  } else if (event->variant == "InstallStarted") {
    const auto install_started = dynamic_cast<event::InstallStarted *>(event.get());
    std::cout << "Installation started for device " << install_started->serial.ToString() << "\n";
  } else if (event->variant == "InstallTargetComplete") {
    const auto install_complete = dynamic_cast<event::InstallTargetComplete *>(event.get());
    std::cout << "Installation complete for device " << install_complete->serial.ToString() << ": "
              << (install_complete->success ? "success" : "failure") << "\n";
  } else if (event->variant == "UpdateCheckComplete") {
    const auto check_complete = dynamic_cast<event::UpdateCheckComplete *>(event.get());
    current_updates = check_complete->result.updates;
    std::cout << current_updates.size() << " updates available\n";
  } else {
    std::cout << "Received " << event->variant << " event\n";
  }
}

int campaign_selection(std::size_t campaign_num_ids)
{
  int retVal = 0;
  std::cout << "Select the Campaign to Accept...\n:";

  while (retVal < 1 || retVal > campaign_num_ids)
  {
    std::cin >> retVal;
    if (retVal < 1 || retVal > campaign_num_ids)
    {
      std::cout << "Enter index number from the selection\n";
    }
  }
  return retVal;
}


int campaignSelection(std::vector<campaign::Campaign>& campaign_list, Aktualizr * aktualizr)
{
  if (campaign_list.size() < 1)
  {
    std::cout << "No Campaigns to Accept...\n";
  }
  else
  {
    int count = 0;
    int campaign_index = 0;
    std::vector<std::string> campaign_id_list;
    std::cout << "--CAMPAIGNS FOUND--...\n";
    for (const auto &c : campaign_list)
    {
      count++;
      std::cout << count << ". " << c.name << "\n";
      std::cout << "--- " << c.id << "\n";
      std::cout << "--- " << c.description << "\n\n";
      campaign_id_list.push_back(c.id);
    }

    campaign_index = campaign_selection(campaign_id_list.size());
    campaign_id_selected = campaign_id_list[campaign_index - 1];
    std::cout << "Campaign Selected: " << campaign_id_selected << "\n";
    (*aktualizr).CampaignControl(campaign_id_selected, campaign::Cmd::Accept);
  }
}

int main(int argc, char *argv[]) {
  logger_init();
  logger_set_threshold(boost::log::trivial::info);
  LOG_INFO << "demo-app starting";

  bpo::variables_map commandline_map = parse_options(argc, argv);

  int r = EXIT_FAILURE;
  boost::signals2::connection conn;

  try {
    Config config(commandline_map);
    LOG_DEBUG << "Current directory: " << boost::filesystem::current_path().string();

    Aktualizr aktualizr(config);
    std::function<void(const std::shared_ptr<event::BaseEvent> event)> f_cb =
        [](const std::shared_ptr<event::BaseEvent> event) { process_event(event); };
    conn = aktualizr.SetSignalHandler(f_cb);

    aktualizr.Initialize();

    std::string buffer;
    while (std::getline(std::cin, buffer)) 
    {
      boost::algorithm::to_lower(buffer);
      if (buffer == "senddevicedata") 
      {
        aktualizr.SendDeviceData();
        std::cout << "\n\nDevice Data Send Test\n\n";
      }
      else if (buffer == "fetchmetadata" || buffer == "fetchmeta" || buffer == "checkupdates" || buffer == "check") 
      {
        aktualizr.CheckUpdates();
      } 
      else if (buffer == "download" || buffer == "startdownload") 
      {
        aktualizr.Download(current_updates);
      }
      else if (buffer == "install" || buffer == "uptaneinstall") 
      {
        aktualizr.Install(current_updates);
        current_updates.clear();
      } 
      else if (buffer == "campaigncheck") 
      {
        auto cc_fut = aktualizr.CampaignCheck();
      } 
      else if (buffer == "pause") 
      {
        aktualizr.Pause();
      } 
      else if (buffer == "resume") 
      {
        aktualizr.Resume();
      } 
      else if (buffer == "abort") 
      {
        aktualizr.Abort();
      } 
      else if (buffer == "lucid") 
      {
        std::cout << "Lucid Air\n";
      } 
      else if (buffer == "uptanecycle") 
      {
        std::cout << "Running Uptane Cycle...\n";
        aktualizr.UptaneCycle();
      }
      else if (buffer == "campaignaccept") 
      {
        std::cout << "Campaign Accepted...\n";
        std::string campaign_id;
        result::CampaignCheck camps = aktualizr.CampaignCheck().get();
        campaignSelection(camps.campaigns, &aktualizr);
      }
      else if (buffer == "sendmanifest")
      {
        std::string update_status = "Complete";
        std::string custom_string;
        custom_manifest["Update Status"] = update_status;
        std::cout << "Enter Custom Text to Send as Manifest: ";

        std::getline(std::cin, custom_string);
        custom_manifest["Custom"] = custom_string;
        std::cout << "Added Custom Message: " << custom_string << "\n";

        aktualizr.SendManifest(custom_manifest);
      }  
      else if (!buffer.empty()) 
      {
        std::cout << "Unknown command\n";
      } 
    }
    r = EXIT_SUCCESS;
  } catch (const std::exception &ex) {
    LOG_ERROR << "Fatal error in demo-app: " << ex.what();
  }

  conn.disconnect();
  return r;
}
