// Copyright (c) 2016 The Chromium Embedded Framework Authors. All rights
// reserved. Use of this source code is governed by a BSD-style license that
// can be found in the LICENSE file.

#include "tests/cefclient/browser/client_browser.h"

#include "include/base/cef_logging.h"
#include "include/cef_command_line.h"
#include "include/cef_crash_util.h"
#include "include/cef_file_util.h"
#include "tests/cefclient/browser/client_prefs.h"
#include "tests/cefclient/browser/default_client_handler.h"
#include "tests/cefclient/browser/main_context.h"
#include "tests/cefclient/browser/root_window_manager.h"
#include "tests/shared/common/client_switches.h"

#if defined(CEF_USE_BOOTSTRAP)
#include "tests/cefclient/browser/client_update_win.h"
#endif

#include <sstream>

#include "include/base/cef_callback.h"
#include "include/cef_request_context.h"
#include "include/wrapper/cef_closure_task.h"

namespace client::browser {

namespace {

// AgentMux test hook: --autocomplete-test="git|rust|weather in" runs each query
// through CefRequestContext::GetAutocompleteSuggestions on the global request
// context and logs every update.
class AutocompleteTestCallback : public CefAutocompleteCallback {
 public:
  explicit AutocompleteTestCallback(const std::string& query)
      : query_(query) {}

  void OnAutocompleteResult(CefRefPtr<CefListValue> matches,
                            bool done) override {
    std::stringstream ss;
    ss << "[autocomplete-test] \"" << query_ << "\" update " << ++updates_
       << (done ? " (done)" : "") << ": " << matches->GetSize() << " matches";
    for (size_t i = 0; i < matches->GetSize() && i < 8; ++i) {
      CefRefPtr<CefDictionaryValue> m = matches->GetDictionary(i);
      ss << "\n    " << m->GetInt("relevance") << " " << m->GetString("type")
         << " [" << m->GetString("provider") << "] \""
         << m->GetString("contents").ToString() << "\" -> "
         << m->GetString("destination_url").ToString();
      const std::string inline_text =
          m->GetString("inline_autocompletion").ToString();
      if (!inline_text.empty()) {
        ss << " (inline: \"" << inline_text << "\")";
      }
    }
    LOG(WARNING) << ss.str();
  }

 private:
  const std::string query_;
  int updates_ = 0;
  IMPLEMENT_REFCOUNTING(AutocompleteTestCallback);
};

void RunAutocompleteTest(const std::string& queries) {
  std::stringstream all(queries);
  std::string query;
  int64_t delay_ms = 5000;  // Let the history index load first.
  while (std::getline(all, query, '|')) {
    if (query.empty()) {
      continue;
    }
    CefPostDelayedTask(TID_UI, base::BindOnce([](const std::string& q) {
                         LOG(WARNING) << "[autocomplete-test] start \"" << q
                                      << "\"";
                         CefRequestContext::GetGlobalContext()
                             ->GetAutocompleteSuggestions(
                                 q, new AutocompleteTestCallback(q));
                       }, query),
                       delay_ms);
    delay_ms += 4000;
  }
}

class ClientBrowserDelegate : public ClientAppBrowser::Delegate {
 public:
  ClientBrowserDelegate() = default;

  ClientBrowserDelegate(const ClientBrowserDelegate&) = delete;
  ClientBrowserDelegate& operator=(const ClientBrowserDelegate&) = delete;

  void OnRegisterCustomPreferences(
      CefRefPtr<client::ClientAppBrowser> app,
      cef_preferences_type_t type,
      CefRawPtr<CefPreferenceRegistrar> registrar) override {
    if (type == CEF_PREFERENCES_TYPE_GLOBAL) {
      // Register global preferences with default values.
      prefs::RegisterGlobalPreferences(registrar);
    }
  }

  void OnContextInitialized(CefRefPtr<ClientAppBrowser> app) override {
    if (CefCrashReportingEnabled()) {
      // Set some crash keys for testing purposes. Keys must be defined in the
      // "crash_reporter.cfg" file. See cef_crash_util.h for details.
      CefSetCrashKeyValue("testkey_small1", "value1_small_browser");
      CefSetCrashKeyValue("testkey_small2", "value2_small_browser");
      CefSetCrashKeyValue("testkey_medium1", "value1_medium_browser");
      CefSetCrashKeyValue("testkey_medium2", "value2_medium_browser");
      CefSetCrashKeyValue("testkey_large1", "value1_large_browser");
      CefSetCrashKeyValue("testkey_large2", "value2_large_browser");
    }

    const std::string& crl_sets_path =
        CefCommandLine::GetGlobalCommandLine()->GetSwitchValue(
            switches::kCRLSetsPath);
    if (!crl_sets_path.empty()) {
      // Load the CRLSets file from the specified path.
      CefLoadCRLSetsFile(crl_sets_path);
    }

    const std::string& autocomplete_test =
        CefCommandLine::GetGlobalCommandLine()->GetSwitchValue(
            "autocomplete-test");
    if (!autocomplete_test.empty()) {
      RunAutocompleteTest(autocomplete_test);
    }

#if defined(CEF_USE_BOOTSTRAP)
    // Confirm launch health after a delay, then check for an update. Ordering
    // the update after confirmation preserves this healthy version as a
    // fallback under the installer's existing pruning policy.
    StartLaunchHealthConfirmation();
#endif
  }

  void OnBeforeCommandLineProcessing(
      CefRefPtr<ClientAppBrowser> app,
      CefRefPtr<CefCommandLine> command_line) override {
    // Append Chromium command line parameters if touch events are enabled
    if (client::MainContext::Get()->TouchEventsEnabled()) {
      command_line->AppendSwitchWithValue("touch-events", "enabled");
    }
  }

  bool OnAlreadyRunningAppRelaunch(
      CefRefPtr<ClientAppBrowser> app,
      CefRefPtr<CefCommandLine> command_line,
      const CefString& current_directory) override {
    // Add logging for some common switches that the user may attempt to use.
    static const char* kIgnoredSwitches[] = {
        switches::kMultiThreadedMessageLoop,
        switches::kOffScreenRenderingEnabled,
        switches::kUseViews,
    };
    for (auto& kIgnoredSwitche : kIgnoredSwitches) {
      if (command_line->HasSwitch(kIgnoredSwitche)) {
        LOG(WARNING) << "The --" << kIgnoredSwitche
                     << " command-line switch is ignored on app relaunch.";
      }
    }

    // Create a new root window based on |command_line|.
    auto config = std::make_unique<RootWindowConfig>(command_line->Copy());

    MainContext::Get()->GetRootWindowManager()->CreateRootWindow(
        std::move(config));

    // Relaunch was handled.
    return true;
  }

  CefRefPtr<CefClient> GetDefaultClient(
      CefRefPtr<ClientAppBrowser> app) override {
    // Default client handler for unmanaged browser windows. Used with
    // Chrome style only.
    LOG(INFO) << "Creating a chrome browser with the default client";
    return new DefaultClientHandler();
  }

 private:
  IMPLEMENT_REFCOUNTING(ClientBrowserDelegate);
};

}  // namespace

void CreateDelegates(ClientAppBrowser::DelegateSet& delegates) {
  delegates.insert(new ClientBrowserDelegate);
}

}  // namespace client::browser
