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
#include "hud/hud_cds.h"
#include "drive_frame.h"
}
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
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
// the Shift point page (state 101): label 160 shows model 54, buttons 161-165 send actions 31-35 = 6200..6600 rpm
const unsigned kBlinkModel = 54u, kBlinkAction = 31u;
const char kLog[] = "/tmp/hud-sportbar.log";
// CDS probe (hud_cds.h): armed by cds.txt; cds.guard exists from registering until kCdsGuardMs later, so a start that
// dies in the CDS calls leaves it behind and the next start skips CDS (delete cds.guard over SSH to retry).
const char kCdsConfig[] = HUD_APP_DIR "/cds.txt";
const char kCdsGuard[] = HUD_APP_DIR "/cds.guard";
const char kCdsLog[] = "/tmp/hud-cds.log";
const unsigned kCdsStartMs = 5000u, kCdsGuardMs = 30000u, kCdsMaxLines = 20000u;
}

class ExtendedHUD;
// One per watched property: the delegate's object, so a value knows its property.
struct CdsSlot {
    ExtendedHUD* app;
    int index;
    bool onValue(const bmw::BasicString<unsigned short>& value);
};

class ExtendedHUD : public bmw::PluginApp {
    friend struct CdsSlot;
    hud_settings set_;
    pid_t worker_;
    bool stopping_;
    unsigned generation_;
    int restarts_;
    uint64_t retry_at_;
    const char* conflict_;
    hud_cds_prop cds_[HUD_CDS_MAX];
    CdsSlot slot_[HUD_CDS_MAX];
    long cdsLast_[HUD_CDS_MAX];          // the last number per property, -1 none yet
    uint64_t cdsSentAt_[HUD_CDS_MAX];
    int ncds_, cdsSock_;
    bool cdsArmed_, cdsOn_, cdsGuard_;
    uint64_t cdsStartAt_, cdsGuardUntil_;
    unsigned cdsLines_;
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
        char blink[64];
        hud_app_blink_label(&set_, blink, sizeof blink);
        updateModel(kBlinkModel, bmw::BasicString<char>(blink));
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
    template <int Choice> bool blink(const bmw::Parcel&) {
        hud_app_set_blink(&set_, Choice);
        apply();
        return true;
    }

public:
    ExtendedHUD(bmw::IDispatcher& d, bmw::BasicString<char> name, unsigned maj, unsigned mn,
                bmw::RHMIInterfaceVersion rhmi, const bmw::Bundle& cfg, const bmw::Bundle& res)
        : bmw::PluginApp(d, name, maj, mn, rhmi, cfg, res), worker_(-1), stopping_(false), generation_(0),
          restarts_(0), retry_at_(0), conflict_(0), ncds_(0), cdsSock_(-1), cdsArmed_(false), cdsOn_(false),
          cdsGuard_(false), cdsStartAt_(0), cdsGuardUntil_(0), cdsLines_(0) {
        hud_app_defaults(&set_);
        status_[0] = 0;
        gateway_[0] = 0;
    }
    virtual ~ExtendedHUD() {
        cdsStop();
        stopWorker();
    }
    virtual bool onFinalize() {
        ++generation_;
        cdsStop();
        stopWorker();
        return true;
    }
    // The framework's own CDS callback (vtable slot 30): logged in case values arrive here and not at the delegates.
    virtual bool invokeCDSUpdate(bmw::BasicString<char>& name, bmw::BasicString<unsigned short>& value) {
        if (cdsOn_) {
            const char* n = name.data();
            cdsLog("update", n ? n : "?", value);
        }
        return bmw::PluginApp::invokeCDSUpdate(name, value);
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
        action.m_exec = &Action::execImpl<ExtendedHUD, &ExtendedHUD::blink<0> >;
        addHandler(kBlinkAction + 0u, action);
        action.m_exec = &Action::execImpl<ExtendedHUD, &ExtendedHUD::blink<1> >;
        addHandler(kBlinkAction + 1u, action);
        action.m_exec = &Action::execImpl<ExtendedHUD, &ExtendedHUD::blink<2> >;
        addHandler(kBlinkAction + 2u, action);
        action.m_exec = &Action::execImpl<ExtendedHUD, &ExtendedHUD::blink<3> >;
        addHandler(kBlinkAction + 3u, action);
        action.m_exec = &Action::execImpl<ExtendedHUD, &ExtendedHUD::blink<4> >;
        addHandler(kBlinkAction + 4u, action);
        close(open(kLog, O_WRONLY | O_CREAT | O_TRUNC, 0644));   // a fresh log per HMI start
        const DiagnosticGateway gateway(HUD_APP_DIR "/gateway-ip.txt");
        strcpy(gateway_, gateway.host());
        hud_app_load(&set_, HUD_APP_DIR);
        conflict_ = hud_app_conflict(HUD_APP_PLUGINS);
        cdsArm();
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
        cdsPoll();
        postPoll();
        return true;
    }

private:
    // ---- CDS probe ----
    void cdsArm() {
        char buf[2048];
        FILE* f = fopen(kCdsConfig, "r");
        if (!f) return;
        size_t n = fread(buf, 1, sizeof buf, f);
        fclose(f);
        ncds_ = hud_cds_parse(buf, n, cds_, HUD_CDS_MAX);
        FILE* log = fopen(kCdsLog, "w");
        if (access(kCdsGuard, F_OK) == 0) {   // the last start did not get past the CDS calls: stay off
            if (log) fprintf(log, "CDS off: %s is left from a start that did not finish; delete it to retry\n", kCdsGuard);
        } else if (ncds_ > 0) {
            cdsArmed_ = true;
            cdsStartAt_ = bd_mono_ms() + kCdsStartMs;   // after the HMI is up, from the poll
            if (log) fprintf(log, "CDS armed: %d properties, registering in %u ms\n", ncds_, kCdsStartMs);
        }
        if (log) fclose(log);
    }
    void cdsPoll() {
        uint64_t now = bd_mono_ms();
        if (cdsArmed_ && !cdsOn_ && now >= cdsStartAt_) cdsStart();
        // CDS sends a value only when it changes (car run 6: the gear went quiet for 108 s, the revs for 32 s), and the
        // worker drops data older than 2 s, so each routed value is repeated: unchanged is still the car's value.
        for (int i = 0; cdsOn_ && i < ncds_; ++i)
            if (cds_[i].kind != HUD_CDS_LOG && cdsLast_[i] >= 0 && now - cdsSentAt_[i] >= 500u) cdsSend(i, cdsLast_[i], now);
        if (cdsGuard_ && now >= cdsGuardUntil_) {
            remove(kCdsGuard);
            cdsGuard_ = false;
            cdsLogLine("guard removed: the CDS calls came through");
        }
    }
    void cdsStart() {
        int g = open(kCdsGuard, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (g >= 0) close(g);
        cdsGuard_ = true;
        cdsGuardUntil_ = bd_mono_ms() + kCdsGuardMs;
        cdsArmed_ = false;
        cdsOn_ = true;
        for (int i = 0; i < HUD_CDS_MAX; ++i) { cdsLast_[i] = -1; cdsSentAt_[i] = 0; }
        cdsSock_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (cdsSock_ >= 0) fcntl(cdsSock_, F_SETFL, fcntl(cdsSock_, F_GETFL) | O_NONBLOCK);
        typedef bmw::Delegate<bool, const bmw::BasicString<unsigned short>&> Watch;
        for (int i = 0; i < ncds_; ++i) {
            char line[160];
            slot_[i].app = this;
            slot_[i].index = i;
            Watch w;
            w.m_obj = &slot_[i];
            w.m_exec = &Watch::execImpl<CdsSlot, &CdsSlot::onValue>;
            bmw::BasicString<char> name(cds_[i].name);
            bool available = cdsIsPropertyAvailable(name);
            snprintf(line, sizeof line, "watch %s kind %d interval %u: available %d", cds_[i].name, (int)cds_[i].kind,
                     cds_[i].interval, available ? 1 : 0);
            cdsLogLine(line);
            cdsAddPropertyChangedEventHandler(name, cds_[i].interval, w);
            cdsGetPropertyAsync(name, w);
        }
    }
    void cdsStop() {
        if (!cdsOn_) return;
        for (int i = 0; i < ncds_; ++i) {
            bmw::BasicString<char> name(cds_[i].name);
            cdsRemovePropertyChangedEventHandler(name);
        }
        cdsOn_ = false;
        if (cdsSock_ >= 0) close(cdsSock_);
        cdsSock_ = -1;
    }
    void cdsLogLine(const char* text) {
        if (cdsLines_ >= kCdsMaxLines) return;
        FILE* log = fopen(kCdsLog, "a");
        if (!log) return;
        fprintf(log, "%llu %s\n", (unsigned long long)bd_mono_ms(), text);
        fclose(log);
        ++cdsLines_;
    }
    void cdsLog(const char* how, const char* name, const bmw::BasicString<unsigned short>& value) {
        char text[HUD_CDS_TEXT], line[HUD_CDS_TEXT + 96];
        long number;
        hud_cds_value(value.data(), HUD_CDS_TEXT, text, sizeof text, &number);
        snprintf(line, sizeof line, "%s %s = %s (%ld)", how, name, text, number);
        cdsLogLine(line);
    }
    bool cdsValue(int i, const bmw::BasicString<unsigned short>& value) {
        if (i < 0 || i >= ncds_ || !cdsOn_) return true;
        char text[HUD_CDS_TEXT];
        long number;
        hud_cds_value(value.data(), HUD_CDS_TEXT, text, sizeof text, &number);
        cdsLog("value", cds_[i].name, value);
        if (cds_[i].kind != HUD_CDS_LOG) cdsLast_[i] = number;
        cdsSend(i, number, bd_mono_ms());
        return true;
    }
    void cdsSend(int i, long number, uint64_t now) {
        struct drive_frame f;
        cdsSentAt_[i] = now;
        if (cdsSock_ >= 0 && hud_cds_frame(cds_[i].kind, number, now, &f)) {
            struct sockaddr_in to;
            memset(&to, 0, sizeof to);
            to.sin_family = AF_INET;
            to.sin_port = htons(HUD_APP_UDP_PORT);
            to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            sendto(cdsSock_, &f, sizeof f, 0, reinterpret_cast<struct sockaddr*>(&to), sizeof to);   // never blocks
        }
    }
};

bool CdsSlot::onValue(const bmw::BasicString<unsigned short>& value) { return app->cdsValue(index, value); }

extern "C" {
PluginAppInfo pluginAppInfo = {1, "ExtendedHUD", "owner-development", 1, create_app<ExtendedHUD>, destroy_app};
}
