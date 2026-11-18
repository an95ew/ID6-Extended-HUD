// ExtendedHUD: My Vehicle -> ExtendedHUD. A thin shell around hud/hud_app.c (host-tested): the menu ticks write the
// settings file, and the plugin keeps the HUD worker (hud-sportbar-qnx, which starts the read-only ShiftDeck collector)
// running while something is ticked. Drawing and the engine reads run in those processes, never in HmiMain3D.
#include "plugin_factory.h"
#include "diagnostic_worker.h"
#include <spawn.h>
#include "diagnostic_gateway.h"
#include "gauge_core.h"
extern "C" {
#include "hud/hud_app.h"
}
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
const unsigned kDataUpdateMessage = 0x40000003u;
const unsigned kPollKey = 0x48534252u;   // HSBR, private to this app
const unsigned kPollMs = 1000u;
const unsigned kRetryMs[] = {2000u, 5000u, 15000u, 60000u};
// ExtendedHUD.xml
const unsigned kStatusModel = 50u, kSportBarModel = 51u, kAlwaysModel = 52u, kNavModel = 53u;
const unsigned kSportBarAction = 21u, kAlwaysAction = 22u, kNavAction = 23u;
const unsigned kAlwaysComponent = 152u;
const char kLog[] = "/tmp/hud-sportbar.log";
}

class ExtendedHUD : public bmw::PluginApp {
    hud_settings set_;
    pid_t worker_;
    bool stopping_;
    unsigned generation_;
    int restarts_;
    uint64_t retry_at_;
    const char* conflict_;
    char status_[128];
    char gateway_[16];

    void postPoll() {
        bmw::Parcel data;
        data.writeUInt32(kPollKey);
        data.writeUInt32(generation_);
        bmw::Message message(kDataUpdateMessage, data);
        post(message, kPollMs);
    }
    void reap() {
        if (worker_ <= 0) return;
        pid_t ended = waitpid(worker_, 0, WNOHANG);
        if (ended == worker_ || (ended < 0 && errno == ECHILD)) {
            if (!stopping_) {   // it ended by itself: try again later, slower each time
                unsigned k = restarts_ < 3 ? restarts_ : 3;
                retry_at_ = bd_mono_ms() + kRetryMs[k];
                ++restarts_;
            }
            worker_ = -1;
            stopping_ = false;
        }
    }
    void stopWorker() {
        reap();
        if (worker_ > 0 && !stopping_) {
            kill(worker_, SIGTERM);   // the worker clears its picture on TERM
            stopping_ = true;
        }
    }
    void startWorker() {
        if (worker_ > 0 || conflict_ || !hud_app_wants_worker(&set_) || bd_mono_ms() < retry_at_) return;
        char buf[1024];
        const char* argv[HUD_APP_MAX_ARGS];
        if (hud_app_worker_argv(HUD_APP_DIR, gateway_, buf, sizeof buf, argv, HUD_APP_MAX_ARGS) < 0) return;
        worker_ = spawnLogged(argv);
        if (worker_ <= 0) {
            worker_ = -1;
            retry_at_ = bd_mono_ms() + kRetryMs[3];
            ++restarts_;
        }
    }
    // Like diagnostic_worker.h's launch(), but stdout and stderr both go to the log (RAM, gone at sleep): the worker
    // reports on stderr, and launch() sends that to /dev/null (2026-11-18: the worker exited and nobody saw why).
    pid_t spawnLogged(const char* const* argv) {
        char library_path[] = "LD_LIBRARY_PATH=/lib:/usr/lib:/boot/lib:/opt/sys/lib";
        char* environment[] = {library_path, 0};
        int null = open("/dev/null", O_RDWR);
        int log = open(kLog, O_WRONLY | O_CREAT | O_APPEND, 0644);
        pid_t child = -1;
        if (null >= 0) {
            int out = log >= 0 ? log : null;
            int descriptors[] = {null, out, out};
            struct inheritance flags;
            memset(&flags, 0, sizeof flags);
            flags.flags = SPAWN_SETSIGDEF | SPAWN_SETSIGMASK;
            sigfillset(&flags.sigdefault);
            sigemptyset(&flags.sigmask);
            child = spawn(argv[0], 3, descriptors, &flags, const_cast<char* const*>(argv), environment);
        }
        if (log >= 0) close(log);
        if (null >= 0) close(null);
        return child;
    }
    void showModels() {
        // "Always" only means something with Sport Bar on: ENABLED (property 1) greys it, SELECTABLE (2) keeps the focus
        // off it. Car 2026-11-18: 2 alone made it unselectable but not grey.
        setProperty(kAlwaysComponent, 1u, bmw::Parcel(set_.sport_bar != 0));
        setProperty(kAlwaysComponent, 2u, bmw::Parcel(set_.sport_bar != 0));
        updateModel(kSportBarModel, set_.sport_bar ? 1 : 0);
        updateModel(kAlwaysModel, set_.always ? 1 : 0);
        updateModel(kNavModel, set_.nav ? 1 : 0);
    }
    void showStatus() {
        char s[128];
        hud_app_status(&set_, worker_ > 0 && !stopping_, conflict_, restarts_, s, sizeof s);
        if (strcmp(s, status_)) {
            strcpy(status_, s);
            updateModel(kStatusModel, bmw::BasicString<char>(s));
        }
    }
    void apply() {
        hud_app_save(&set_, HUD_APP_DIR);   // the worker re-reads it every second
        if (!hud_app_wants_worker(&set_)) stopWorker();
        else {
            restarts_ = 0;
            retry_at_ = 0;
            startWorker();
        }
        showModels();
        showStatus();
    }
    template <hud_app_item Item> bool toggle(const bmw::Parcel&) {
        hud_app_toggle(&set_, Item);
        apply();
        return true;
    }

public:
    ExtendedHUD(bmw::IDispatcher& d, bmw::BasicString<char> name, unsigned maj, unsigned mn,
                bmw::RHMIInterfaceVersion rhmi, const bmw::Bundle& cfg, const bmw::Bundle& res)
        : bmw::PluginApp(d, name, maj, mn, rhmi, cfg, res), worker_(-1), stopping_(false), generation_(0),
          restarts_(0), retry_at_(0), conflict_(0) {
        hud_app_defaults(&set_);
        status_[0] = 0;
        gateway_[0] = 0;
    }
    virtual ~ExtendedHUD() { stopWorker(); }
    virtual bool onFinalize() {
        ++generation_;
        stopWorker();
        return true;
    }
    virtual bool onInit() {
        typedef bmw::Delegate<bool, const bmw::Parcel&> Action;
        Action action;
        action.m_obj = this;
        action.m_exec = &Action::execImpl<ExtendedHUD, &ExtendedHUD::toggle<HUD_APP_SPORT_BAR> >;
        addHandler(kSportBarAction, action);
        action.m_exec = &Action::execImpl<ExtendedHUD, &ExtendedHUD::toggle<HUD_APP_ALWAYS> >;
        addHandler(kAlwaysAction, action);
        action.m_exec = &Action::execImpl<ExtendedHUD, &ExtendedHUD::toggle<HUD_APP_NAV> >;
        addHandler(kNavAction, action);
        close(open(kLog, O_WRONLY | O_CREAT | O_TRUNC, 0644));   // a fresh log per HMI start
        const DiagnosticGateway gateway(HUD_APP_DIR "/gateway-ip.txt");
        strcpy(gateway_, gateway.host());
        hud_app_load(&set_, HUD_APP_DIR);
        conflict_ = hud_app_conflict(HUD_APP_PLUGINS);
        startWorker();
        showModels();
        showStatus();
        ++generation_;
        postPoll();
        return true;
    }
    virtual bool invokeDataUpdate(unsigned key, const bmw::Parcel& data) {
        if (key != kPollKey) return bmw::PluginApp::invokeDataUpdate(key, data);
        unsigned generation;
        if (!data.readUInt32(generation)) return false;
        if (generation != generation_) return true;
        reap();
        startWorker();
        showStatus();
        postPoll();
        return true;
    }
};

extern "C" {
PluginAppInfo pluginAppInfo = {1, "ExtendedHUD", "owner-development", 1, create_app<ExtendedHUD>, destroy_app};
}
