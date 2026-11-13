// The command channel (docs/adr/0007-engine-command-channel.md): the control
// messages and their codec, RunSettings validation, the RiskManager kill
// switch, ConfiguredRun with its stage metrics and results export, the live
// feed loop, and EngineService taking commands over the in-memory broker
// exactly as `trading_engine serve` takes them over Kafka.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "trading_engine/analytics/performance_analyzer.hpp"
#include "trading_engine/analytics/results_export.hpp"
#include "trading_engine/app/configured_run.hpp"
#include "trading_engine/app/engine_service.hpp"
#include "trading_engine/app/live_feed.hpp"
#include "trading_engine/common/clock.hpp"
#include "trading_engine/common/timestamp_text.hpp"
#include "trading_engine/control/control_codec.hpp"
#include "trading_engine/events/event_bus.hpp"
#include "trading_engine/events/event_codec.hpp"
#include "trading_engine/events/message_transport.hpp"

namespace {

namespace ev  = trading_engine::events;
namespace dom = trading_engine::domain;
namespace tec = trading_engine::common;
namespace app = trading_engine::app;
namespace ctl = trading_engine::control;
namespace an  = trading_engine::analytics;
namespace md  = trading_engine::market_data;
using namespace std::chrono;

tec::Timestamp at(const char* text) { return *tec::parse_timestamp(text); }

std::vector<dom::MarketEvent> bars(int minutes_count = 120) {
    std::vector<dom::MarketEvent> out;
    const tec::Timestamp start{sys_days{2026y / January / 5} + 14h + 30min};
    for (int i = 0; i < minutes_count; ++i) {
        for (const char* symbol : {"AAPL", "MSFT"}) {
            const double base  = symbol[0] == 'A' ? 100.0 : 300.0;
            const double price = base + 8.0 * std::sin(i / 4.0) + 0.05 * i + (i % 7 == 0 ? 6.0 : 0.0);
            dom::MarketEvent e;
            e.symbol        = symbol;
            e.type          = dom::MarketEventType::Bar;
            e.exchange_time = start + minutes{i};
            e.price         = tec::Price::from_double(std::round(price * 100.0) / 100.0);
            out.push_back(e);
        }
    }
    return out;
}

ctl::RunSettings settings() {
    ctl::RunSettings s;
    s.name = "test";
    ctl::StrategySettings sma;
    sma.kind         = ctl::StrategyKind::SmaCrossover;
    sma.strategy_id  = "fast_sma";
    sma.symbols      = {"AAPL", "MSFT"};
    sma.short_window = 3;
    sma.long_window  = 8;
    ctl::StrategySettings mr;
    mr.kind            = ctl::StrategyKind::MeanReversion;
    mr.strategy_id     = "mr";
    mr.symbols         = {"AAPL"};
    mr.lookback        = 10;
    mr.entry_threshold = 1.5;
    s.strategies = {sma, mr};
    s.risk.max_order_quantity = 500;
    s.risk.max_daily_loss     = tec::Money::from_units(1234);
    s.simulation.starting_cash = tec::Money::from_units(50'000);
    s.simulation.slippage_bps  = 2.5;
    s.simulation.fees.per_share = tec::Money::from_double(0.005);
    return s;
}

// --- codec ---------------------------------------------------------------------------

TEST(ControlCodec, CommandRoundTripsWithTheConsoleRunConfig) {
    ctl::Command c;
    c.command_id = "cmd-1";
    c.run_id     = "te.bt-1";
    c.issued_at  = at("2026-10-30T10:00:00Z");
    c.settings   = settings();
    c.settings->period_start = at("2026-01-05T13:30:00Z");
    const std::string text = ctl::encode({"gw", 3, at("2026-10-30T10:00:01Z"), c});
    EXPECT_NE(text.find(R"("type":"command")"), std::string::npos);
    EXPECT_NE(text.find(R"("max_daily_loss":"1234")"), std::string::npos);
    EXPECT_NE(text.find(R"("per_share":"0.005")"), std::string::npos);
    EXPECT_NE(text.find(R"("allow_short":false)"), std::string::npos);

    const auto out = ctl::decode_control(text);
    EXPECT_EQ(out.origin, "gw");
    EXPECT_EQ(out.sequence, 3U);
    const auto& d = std::get<ctl::Command>(out.payload);
    EXPECT_EQ(d.action, ctl::CommandAction::StartBacktest);
    EXPECT_EQ(d.run_id, "te.bt-1");
    ASSERT_TRUE(d.settings.has_value());
    EXPECT_EQ(ctl::settings_json(*d.settings), ctl::settings_json(*c.settings));
    EXPECT_EQ(d.settings->strategies[1].kind, ctl::StrategyKind::MeanReversion);
    EXPECT_EQ(d.settings->strategies[1].entry_threshold, 1.5);
    EXPECT_EQ(d.settings->simulation.fees.per_share, tec::Money::from_double(0.005));
}

TEST(ControlCodec, StartLiveRoundTripsWithItsFeed) {
    ctl::Command c;
    c.command_id = "cmd-2";
    c.action     = ctl::CommandAction::StartLive;
    c.run_id     = "te.live-1";
    c.settings   = settings();
    c.settings->mode      = trading_engine::config::RunMode::Live;
    c.settings->live_feed = "sip";
    const std::string text = ctl::encode({"gw", 1, at("2026-10-30T10:00:01Z"), c});
    EXPECT_NE(text.find(R"("action":"start_live")"), std::string::npos);
    const auto d = std::get<ctl::Command>(ctl::decode_control(text).payload);
    EXPECT_EQ(d.action, ctl::CommandAction::StartLive);
    ASSERT_TRUE(d.settings.has_value());
    EXPECT_EQ(d.settings->mode, trading_engine::config::RunMode::Live);
    EXPECT_EQ(d.settings->live_feed, "sip");
}

TEST(ControlCodec, ReadsWhatTheConsoleSends) {
    // A BacktestRequest as web/src/pages/NewBacktest.tsx builds it, wrapped by the gateway.
    const std::string text = R"({"schema":1,"origin":"gw","sequence":1,"type":"command",
      "enqueued_at":"2026-10-30T10:00:00Z","payload":{"command_id":"c1","action":"start_backtest",
      "run_id":"te.bt-2","issued_at":"2026-10-30T10:00:00Z","settings":{"name":"Probe","mode":"backtest",
      "period_start":"2026-01-05T13:30:00Z","period_end":"2026-01-05T20:00:00Z",
      "strategies":[{"kind":"sma_crossover","strategy_id":"sma_crossover","symbols":["AAPL"],
        "requested_quantity":10,"short_window":5,"long_window":20}],
      "risk":{"max_order_quantity":1000,"max_position_notional":"25000","max_gross_exposure":"100000",
        "max_daily_loss":"5000","max_open_orders":50,"allow_short":false},
      "simulation":{"starting_cash":"100000","slippage_bps":1,"fees":{"per_share":"0","per_trade":"0","bps":0},
        "latency_ms":{"signal_to_order":1,"order_to_fill":5},"participation_cap":0.1}}}})";
    const auto c = std::get<ctl::Command>(ctl::decode_control(text).payload);
    ASSERT_TRUE(c.settings.has_value()) << c.settings_error;
    EXPECT_EQ(c.settings->strategies[0].requested_quantity, 10);
    EXPECT_EQ(c.settings->participation_cap, 0.1);
    EXPECT_EQ(c.settings->simulation.latency.order_to_fill, milliseconds{5});
    EXPECT_NO_THROW(ctl::validate(*c.settings));
}

TEST(ControlCodec, AnUnreadableSettingIsReportedWithItsPath) {
    const std::string text = R"({"schema":1,"origin":"gw","sequence":1,"type":"command",
      "enqueued_at":"2026-10-30T10:00:00Z","payload":{"command_id":"c1","action":"start_backtest",
      "run_id":"te.bt-3","issued_at":"2026-10-30T10:00:00Z","settings":{"strategies":[],
      "risk":{"max_order_quantity":1,"max_position_notional":"1","max_gross_exposure":"1",
        "max_daily_loss":"1","max_open_orders":1,"allow_short":true}}}})";
    const auto c = std::get<ctl::Command>(ctl::decode_control(text).payload);
    EXPECT_FALSE(c.settings.has_value());
    EXPECT_EQ(c.settings_error_field, "risk.allow_short");

    EXPECT_THROW((void)ctl::decode_control(R"({"schema":1,"origin":"x","sequence":1,"type":"nope",
      "enqueued_at":"2026-10-30T10:00:00Z","payload":{}})"), ev::DecodeError);
    EXPECT_THROW((void)ctl::decode_control("[]"), ev::DecodeError);
}

TEST(ControlCodec, StatusReportAndAnnouncementRoundTrip) {
    ctl::RunStatusReport r;
    r.run_id      = "te.bt-1";
    r.engine_id   = "engine-a";
    r.status      = ctl::RunState::Running;
    r.updated_at  = at("2026-10-30T10:00:00Z");
    r.settings    = settings();
    r.kill_switch = {true, at("2026-01-05T15:00:00Z"), "manual"};
    r.progress    = 0.25;
    r.clock       = at("2026-01-05T15:00:00Z");
    r.command_id  = "cmd-9";
    const auto out = std::get<ctl::RunStatusReport>(ctl::decode_control(ctl::encode({"e", 1, r.updated_at, r})).payload);
    EXPECT_EQ(out.status, ctl::RunState::Running);
    EXPECT_TRUE(out.kill_switch.engaged);
    EXPECT_EQ(out.kill_switch.reason, "manual");
    EXPECT_EQ(out.progress, 0.25);
    EXPECT_EQ(out.command_id, "cmd-9");
    EXPECT_FALSE(out.error.has_value());
    EXPECT_EQ(ctl::settings_json(out.settings), ctl::settings_json(r.settings));

    ctl::EngineAnnouncement a;
    a.engine_id = "engine-a";
    a.version   = "0.1.0";
    a.symbols   = {"AAPL", "MSFT"};
    a.first_session = "2026-01-05";
    a.last_session  = "2026-01-06";
    a.runs          = {"te.bt-1"};
    a.heartbeat_ms  = 2000;
    const std::string text = ctl::encode({"e", 2, r.updated_at, a});
    EXPECT_NE(text.find(R"("market_data":{"symbols":["AAPL","MSFT"],"first_session":"2026-01-05")"), std::string::npos);
    const auto back = std::get<ctl::EngineAnnouncement>(ctl::decode_control(text).payload);
    EXPECT_EQ(back.runs, a.runs);
    EXPECT_EQ(back.heartbeat_ms, 2000U);
}

TEST(RunSettingsValidation, NamesTheFieldTheFormShows) {
    auto expect_field = [](ctl::RunSettings s, const char* field) {
        try {
            ctl::validate(s);
            ADD_FAILURE() << "expected " << field;
        } catch (const ctl::SettingsError& e) {
            EXPECT_EQ(e.field(), field) << e.message();
        }
    };
    EXPECT_NO_THROW(ctl::validate(settings()));
    auto s = settings();
    s.strategies[0].long_window = 2;
    expect_field(s, "strategies.0.long_window");
    s = settings();
    s.strategies[1].strategy_id = "fast_sma";
    expect_field(s, "strategies.1.strategy_id");
    s = settings();
    s.strategies.clear();
    expect_field(s, "strategies");
    s = settings();
    s.simulation.starting_cash = tec::Money{};
    expect_field(s, "simulation.starting_cash");
    s = settings();
    s.period_start = at("2026-01-06T00:00:00Z");
    s.period_end   = at("2026-01-05T00:00:00Z");
    expect_field(s, "period");
    s = settings();
    s.mode      = trading_engine::config::RunMode::Live;
    s.live_feed = "nyse";
    expect_field(s, "feed");
    s.live_feed = "test";
    EXPECT_NO_THROW(ctl::validate(s));
}

// --- the kill switch, through a whole run ------------------------------------------------

struct Counted {
    std::vector<dom::RiskDecisionReport> decisions;
    std::size_t fills{0};
};

Counted run_with_kill_switch(bool engaged) {
    const auto input = app::ConfiguredRun::select_bars(bars(), settings());
    tec::ManualClock clock{input.front().exchange_time};
    ev::InProcessEventBus bus{clock};
    Counted counted;
    bus.subscribe(ev::EventType::RiskDecision, [&](const ev::Event& e) {
        counted.decisions.push_back(std::get<dom::RiskDecisionReport>(e.payload));
    });
    bus.subscribe(ev::EventType::Fill, [&](const ev::Event&) { ++counted.fills; });
    app::ConfiguredRun run{bus, clock, settings()};
    if (engaged) {
        const auto k = run.flow().risk().set_kill_switch(true, "fat finger");
        EXPECT_TRUE(k.engaged);
        EXPECT_TRUE(k.changed_at.has_value());
    }
    EXPECT_TRUE(run.replay(input));
    return counted;
}

TEST(KillSwitch, RejectsEverySignalWhileEngaged) {
    const auto normal = run_with_kill_switch(false);
    ASSERT_FALSE(normal.decisions.empty());
    EXPECT_GT(normal.fills, 0U);

    const auto killed = run_with_kill_switch(true);
    EXPECT_EQ(killed.decisions.size(), normal.decisions.size());   // same signals, all refused
    EXPECT_EQ(killed.fills, 0U);
    for (const auto& d : killed.decisions) {
        EXPECT_EQ(d.outcome, dom::RiskOutcome::Rejected);
        EXPECT_EQ(d.reason, "kill_switch");
        ASSERT_EQ(d.checks.size(), 1U);
        EXPECT_EQ(d.checks[0].detail, "kill switch engaged: fat finger");
    }
}

TEST(KillSwitch, ReleaseClearsTheReason) {
    const auto input = bars(2);
    tec::ManualClock clock{input.front().exchange_time};
    ev::InProcessEventBus bus{clock};
    app::ConfiguredRun run{bus, clock, settings()};
    (void)run.flow().risk().set_kill_switch(true, "why");
    const auto released = run.flow().risk().set_kill_switch(false, "ignored");
    EXPECT_FALSE(released.engaged);
    EXPECT_TRUE(released.reason.empty());
    EXPECT_FALSE(run.flow().risk().kill_switch().engaged);
}

TEST(ConfiguredRun, SelectsTheSettingsSymbolsAndPeriodAndCanStopEarly) {
    auto s = settings();
    s.strategies.pop_back();
    s.strategies[0].symbols = {"MSFT"};
    s.period_start = at("2026-01-05T15:00:00Z");
    s.period_end   = at("2026-01-05T15:09:00Z");
    const auto input = app::ConfiguredRun::select_bars(bars(), s);
    ASSERT_EQ(input.size(), 10U);
    for (const auto& b : input) {
        EXPECT_EQ(b.symbol, "MSFT");
    }

    tec::ManualClock clock{input.front().exchange_time};
    ev::InProcessEventBus bus{clock};
    app::ConfiguredRun run{bus, clock, s};
    std::size_t instants = 0;
    EXPECT_FALSE(run.replay(input, [&](const app::ReplayProgress& p) {
        ++instants;
        EXPECT_EQ(p.total, 10U);
        return p.done < 4;
    }));
    EXPECT_EQ(instants, 4U);
    EXPECT_EQ(run.pipeline().market_data().stats().accepted, 4U);
}

// A live feed is the same run with the bars arriving one by one.
TEST(ConfiguredRun, FeedingBarsOneByOneEndsWhereTheReplayEnds) {
    const auto s = settings();
    const auto input = app::ConfiguredRun::select_bars(bars(), s);

    tec::ManualClock replay_clock{input.front().exchange_time};
    ev::InProcessEventBus replay_bus{replay_clock};
    app::ConfiguredRun replayed{replay_bus, replay_clock, s};
    ASSERT_TRUE(replayed.replay(input));

    tec::ManualClock live_clock{input.front().exchange_time};
    ev::InProcessEventBus live_bus{live_clock};
    app::ConfiguredRun fed{live_bus, live_clock, s};
    fed.begin(input.front().exchange_time);
    for (const auto& bar : input) {
        fed.feed(bar);
    }

    const auto a = replayed.pipeline().portfolio().snapshot();
    const auto b = fed.pipeline().portfolio().snapshot();
    EXPECT_EQ(a.cash, b.cash);
    EXPECT_EQ(a.total_equity, b.total_equity);
    EXPECT_EQ(replayed.pipeline().strategies().stats().signals_published,
              fed.pipeline().strategies().stats().signals_published);
    EXPECT_EQ(fed.pipeline().market_data().stats().accepted, input.size());

    fed.feed(input.front());   // an older event never moves the clock back
    EXPECT_EQ(live_clock.now(), input.back().exchange_time);
    fed.end();
}

TEST(ConfiguredRun, MetricsCountEveryStage) {
    const auto s     = settings();
    const auto input = app::ConfiguredRun::select_bars(bars(), s);
    tec::ManualClock clock{input.front().exchange_time};
    ev::InProcessEventBus bus{clock};
    an::SystemMetrics metrics;
    app::ConfiguredRun run{bus, clock, s, tec::RunId{1}, &metrics};
    ASSERT_TRUE(run.replay(input));
    const auto m = metrics.snapshot();
    for (const auto stage : {an::Stage::Ingest, an::Stage::Strategy, an::Stage::Risk, an::Stage::Execution,
                             an::Stage::Portfolio}) {
        const auto& st = m.stages[static_cast<std::size_t>(stage)];
        EXPECT_GT(st.events, 0U) << an::to_string(stage);
        EXPECT_EQ(st.errors, 0U) << an::to_string(stage);
        EXPECT_EQ(st.latency.count, stage == an::Stage::Ingest ? st.latency.count : st.events);
    }
    EXPECT_EQ(m.stages[static_cast<std::size_t>(an::Stage::Ingest)].events, input.size());
    EXPECT_EQ(m.stages[static_cast<std::size_t>(an::Stage::Persistence)].events, 0U);
}

TEST(ResultsExport, WritesTheSummaryCurveFillsAndStages) {
    const auto s     = settings();
    const auto input = app::ConfiguredRun::select_bars(bars(), s);
    tec::ManualClock clock{input.front().exchange_time};
    ev::InProcessEventBus bus{clock};
    an::RunResults r;
    bus.subscribe(ev::EventType::Fill, [&](const ev::Event& e) { r.fills.push_back(std::get<dom::Fill>(e.payload)); });
    an::SystemMetrics metrics;
    app::ConfiguredRun run{bus, clock, s, tec::RunId{1}, &metrics};
    r.equity_curve.push_back(run.pipeline().portfolio().snapshot());
    ASSERT_TRUE(run.replay(input, [&](const app::ReplayProgress&) {
        r.equity_curve.push_back(run.pipeline().portfolio().snapshot());
        return true;
    }));
    r.name    = "te.bt-x";
    r.mode    = "backtest";
    r.source  = "bars \"quoted\".csv";
    r.symbols = {"AAPL", "MSFT"};
    r.report  = an::PerformanceAnalyzer{}.analyze(r.equity_curve, r.fills);
    r.metrics = metrics.snapshot();
    ASSERT_FALSE(r.fills.empty());

    std::ostringstream out;
    an::write_results_json(out, r);
    const std::string json = out.str();
    const std::string source = R"("source": "bars \"quoted\".csv")";
    const std::string first  = R"({"time": "2026-01-05T14:30:00.000000000Z", "equity": 50000, "cash": 50000})";
    EXPECT_NE(json.find(R"("schema": "trading_engine.results/1")"), std::string::npos);
    EXPECT_NE(json.find(source), std::string::npos) << json.substr(0, 200);
    EXPECT_NE(json.find(first), std::string::npos) << json.substr(0, 1200);
    EXPECT_NE(json.find(R"("sharpe": )"), std::string::npos);
    EXPECT_NE(json.find(R"("stage": "strategy")"), std::string::npos);
    EXPECT_EQ(json.find(R"("stage": "persistence")"), std::string::npos);
    std::size_t fills = 0;
    for (auto at_ = json.find("\"order_id\""); at_ != std::string::npos; at_ = json.find("\"order_id\"", at_ + 1)) {
        ++fills;
    }
    EXPECT_EQ(fills, r.fills.size());
}

// A live source double: pushes `events` from its own thread, then either
// keeps running until stopped or gives up with `failure`.
class FakeLiveSource final : public md::IMarketDataSource {
public:
    FakeLiveSource(md::IMarketEventSink& sink, std::vector<dom::MarketEvent> events, std::optional<std::string> failure)
        : sink_{sink}, events_{std::move(events)}, failure_{std::move(failure)} {}
    ~FakeLiveSource() override { stop(); }

    void start() override {
        running_ = true;
        thread_  = std::thread{[this] {
            for (const auto& e : events_) {
                sink_.on_market_event(e);
            }
            if (failure_) {
                running_ = false;
            }
        }};
    }
    void stop() override {
        running_ = false;
        if (thread_.joinable()) {
            thread_.join();
        }
    }
    [[nodiscard]] bool is_running() const override { return running_; }
    [[nodiscard]] std::optional<std::string> failure() const { return failure_; }

private:
    md::IMarketEventSink&         sink_;
    std::vector<dom::MarketEvent> events_;
    std::optional<std::string>    failure_;
    std::atomic<bool>             running_{false};
    std::thread                   thread_;
};

app::LiveFeed fake_feed(md::IMarketEventSink& sink, std::vector<dom::MarketEvent> events,
                        std::optional<std::string> failure = std::nullopt) {
    auto source  = std::make_unique<FakeLiveSource>(sink, std::move(events), std::move(failure));
    auto* handle = source.get();
    return {std::move(source), [handle] { return handle->failure(); }};
}

TEST(LiveFeed, FeedsEveryEventUntilTheSourceGivesUp) {
    const auto s     = settings();
    const auto input = app::ConfiguredRun::select_bars(bars(30), s);
    tec::ManualClock clock{input.front().exchange_time};
    ev::InProcessEventBus bus{clock};
    app::ConfiguredRun run{bus, clock, s};
    app::LiveEventQueue queue;
    auto feed = fake_feed(queue, input, "connection refused");
    std::size_t fed = 0;
    app::LiveHooks hooks;
    hooks.after_feed = [&](const dom::MarketEvent&) { ++fed; };
    EXPECT_EQ(app::feed_live(run, feed, queue, input.front().exchange_time, hooks), "connection refused");
    EXPECT_EQ(fed, input.size());
    EXPECT_EQ(run.pipeline().market_data().stats().accepted, input.size());
}

// --- EngineService over the in-memory broker ----------------------------------------------

class EngineServiceTest : public ::testing::Test {
protected:
    std::shared_ptr<ev::InMemoryBroker> broker_ = std::make_shared<ev::InMemoryBroker>();
    tec::SystemClock wall_;
    std::uint64_t sequence_{0};

    std::unique_ptr<app::EngineService> make(app::EngineServiceConfig config = {}) {
        config.bars = bars();
        config.engine_id = "engine-test";
        config.poll_interval = milliseconds{5};
        return std::make_unique<app::EngineService>(
            [this](std::string_view) { return std::make_unique<ev::InMemoryTransport>(broker_); }, wall_, config);
    }

    void send(ctl::Command command) {
        command.issued_at = wall_.now();
        ev::InMemoryTransport gateway{broker_};
        ASSERT_TRUE(gateway.send({ctl::command_topic(ctl::kDefaultControlPrefix), command.run_id,
                                  ctl::encode({"gateway", ++sequence_, wall_.now(), command})}));
    }

    std::vector<ctl::RunStatusReport> reports(const std::string& run_id) const {
        std::vector<ctl::RunStatusReport> out;
        for (const auto& m : broker_->messages(ctl::run_topic(run_id))) {
            out.push_back(std::get<ctl::RunStatusReport>(ctl::decode_control(m.payload).payload));
        }
        return out;
    }

    // Waits for a report answering `command_id`.
    std::optional<ctl::RunStatusReport> answer(const std::string& run_id, const std::string& command_id) const {
        for (int i = 0; i < 400; ++i) {
            for (const auto& r : reports(run_id)) {
                if (r.command_id == command_id) {
                    return r;
                }
            }
            std::this_thread::sleep_for(milliseconds{5});
        }
        return std::nullopt;
    }

    static ctl::Command start(const std::string& run_id, const std::string& id) {
        ctl::Command c;
        c.command_id = id;
        c.action     = ctl::CommandAction::StartBacktest;
        c.run_id     = run_id;
        c.settings   = settings();
        return c;
    }
    static ctl::Command kill(const std::string& run_id, const std::string& id, bool engaged) {
        ctl::Command c;
        c.command_id = id;
        c.action     = ctl::CommandAction::SetKillSwitch;
        c.run_id     = run_id;
        c.engaged    = engaged;
        c.reason     = "from the console";
        return c;
    }
};

TEST_F(EngineServiceTest, StartsABacktestAndReportsItsSettingsAndProgress) {
    auto service = make();
    service->start();
    send(start("te.bt-a", "c1"));
    const auto queued = answer("te.bt-a", "c1");
    ASSERT_TRUE(queued.has_value());
    EXPECT_FALSE(queued->error.has_value());
    EXPECT_EQ(queued->engine_id, "engine-test");
    ASSERT_TRUE(service->wait_until_idle(seconds{20}));

    const auto all = reports("te.bt-a");
    ASSERT_GE(all.size(), 3U);
    EXPECT_EQ(all.front().status, ctl::RunState::Queued);
    EXPECT_EQ(all.back().status, ctl::RunState::Completed);
    EXPECT_EQ(all.back().progress, 1.0);
    // The settings it ran with, with the period filled in from the bars.
    EXPECT_EQ(all.back().settings.risk.max_daily_loss, tec::Money::from_units(1234));
    EXPECT_EQ(all.back().settings.strategies[0].strategy_id, "fast_sma");
    EXPECT_EQ(all.back().settings.period_start, bars().front().exchange_time);

    // The run's own topics carry the run, like a `trading_engine run`.
    EXPECT_EQ(broker_->messages("te.bt-a.market_data").size(), bars().size());
    EXPECT_FALSE(broker_->messages("te.bt-a.fill").empty());
    const auto fill = std::get<dom::Fill>(ev::decode(broker_->messages("te.bt-a.fill").front().payload).event.payload);
    EXPECT_GT(fill.fees, tec::Money{});   // the settings' per-share fee

    const auto beats = broker_->messages(ctl::engine_topic(ctl::kDefaultControlPrefix));
    ASSERT_FALSE(beats.empty());
    const auto a = std::get<ctl::EngineAnnouncement>(ctl::decode_control(beats.front().payload).payload);
    EXPECT_EQ(a.symbols, (std::vector<std::string>{"AAPL", "MSFT"}));
    EXPECT_EQ(a.first_session, "2026-01-05");
    EXPECT_EQ(service->stats().completed, 1U);
}

TEST_F(EngineServiceTest, RefusesBadSettingsOnTheRunTopic) {
    auto service = make();
    service->start();
    auto c = start("te.bt-bad", "c1");
    c.settings->strategies[0].long_window = 1;
    send(c);
    const auto r = answer("te.bt-bad", "c1");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, ctl::RunState::Failed);
    ASSERT_TRUE(r->error.has_value());
    EXPECT_EQ(r->error->rfind("strategies.0.long_window: ", 0), 0U) << *r->error;

    auto none = start("te.bt-none", "c2");
    none.settings->period_start = at("2027-01-01T00:00:00Z");
    send(none);
    const auto empty = answer("te.bt-none", "c2");
    ASSERT_TRUE(empty.has_value());
    EXPECT_EQ(empty->status, ctl::RunState::Failed);
}

TEST_F(EngineServiceTest, AKillSwitchEngagedWhileQueuedHoldsForTheWholeRun) {
    app::EngineServiceConfig config;
    config.instant_delay = milliseconds{20};
    auto service = make(config);
    service->start();
    send(start("te.bt-first", "c1"));    // keeps the worker busy
    send(start("te.bt-second", "c2"));
    ASSERT_TRUE(answer("te.bt-second", "c2").has_value());
    send(kill("te.bt-second", "c3", true));
    const auto engaged = answer("te.bt-second", "c3");
    ASSERT_TRUE(engaged.has_value());
    EXPECT_EQ(engaged->status, ctl::RunState::Queued);
    EXPECT_TRUE(engaged->kill_switch.engaged);
    EXPECT_EQ(engaged->kill_switch.reason, "from the console");

    ctl::Command stop;
    stop.command_id = "c4";
    stop.action     = ctl::CommandAction::StopRun;
    stop.run_id     = "te.bt-first";
    send(stop);
    const auto stopped = answer("te.bt-first", "c4");
    ASSERT_TRUE(stopped.has_value());
    EXPECT_EQ(stopped->status, ctl::RunState::Stopped);
    EXPECT_LT(stopped->progress.value_or(1.0), 1.0);

    ASSERT_TRUE(service->wait_until_idle(seconds{30}));
    EXPECT_EQ(reports("te.bt-second").back().status, ctl::RunState::Completed);
    const auto decisions = broker_->messages("te.bt-second.risk_decision");
    ASSERT_FALSE(decisions.empty());
    for (const auto& m : decisions) {
        const auto d = std::get<dom::RiskDecisionReport>(ev::decode(m.payload).event.payload);
        EXPECT_EQ(d.reason, "kill_switch");
    }
    EXPECT_TRUE(broker_->messages("te.bt-second.fill").empty());

    // Commands for a finished run are answered with an error; unknown runs are ignored.
    send(kill("te.bt-second", "c5", false));
    const auto late = answer("te.bt-second", "c5");
    ASSERT_TRUE(late.has_value());
    EXPECT_EQ(late->error, "The run has already finished.");
    send(kill("te.bt-elsewhere", "c6", true));
    std::this_thread::sleep_for(milliseconds{100});
    EXPECT_TRUE(reports("te.bt-elsewhere").empty());
    EXPECT_GE(service->stats().ignored, 1U);
}

TEST_F(EngineServiceTest, AKillSwitchEngagedMidRunRejectsFromThenOn) {
    app::EngineServiceConfig config;
    config.instant_delay = milliseconds{15};
    auto service = make(config);
    service->start();
    send(start("te.bt-mid", "c1"));
    for (int i = 0; i < 400; ++i) {   // until it is partway through
        const auto all = reports("te.bt-mid");
        if (!all.empty() && all.back().status == ctl::RunState::Running && all.back().progress.value_or(0) > 0.3) {
            break;
        }
        std::this_thread::sleep_for(milliseconds{10});
    }
    send(kill("te.bt-mid", "c2", true));
    const auto engaged = answer("te.bt-mid", "c2");
    ASSERT_TRUE(engaged.has_value());
    EXPECT_EQ(engaged->status, ctl::RunState::Running);
    ASSERT_TRUE(engaged->kill_switch.changed_at.has_value());   // stamped in market time
    const auto when = *engaged->kill_switch.changed_at;
    ASSERT_TRUE(service->wait_until_idle(seconds{30}));

    std::size_t before = 0, after = 0;
    for (const auto& m : broker_->messages("te.bt-mid.risk_decision")) {
        const auto d = std::get<dom::RiskDecisionReport>(ev::decode(m.payload).event.payload);
        if (d.decided_at > when) {
            EXPECT_EQ(d.reason, "kill_switch");
            ++after;
        } else if (d.reason != "kill_switch") {
            ++before;
        }
    }
    EXPECT_GT(before, 0U);
    EXPECT_GT(after, 0U);
}

TEST_F(EngineServiceTest, RefusesALiveRunWithoutALiveFeed) {
    auto service = make();
    service->start();
    auto c   = start("te.live-none", "c1");
    c.action = ctl::CommandAction::StartLive;
    send(c);
    const auto r = answer("te.live-none", "c1");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->status, ctl::RunState::Failed);
    EXPECT_EQ(r->error, "This engine has no live feed; start it with Alpaca credentials set.");
}

TEST_F(EngineServiceTest, RunsALiveRunBesideTheQueueUntilStopped) {
    std::vector<std::string> finished;
    std::mutex finished_mutex;
    std::vector<tec::Symbol> asked;
    app::EngineServiceConfig config;
    config.live_feed = [&](const std::vector<tec::Symbol>& symbols, const std::string& feed,
                           md::IMarketEventSink& sink, const tec::IClock&) {
        asked = symbols;
        EXPECT_EQ(feed, "test");
        return fake_feed(sink, bars(20));
    };
    config.on_run_finished = [&](const std::string& run_id, const an::SystemMetrics::Snapshot& m) {
        const std::lock_guard lock{finished_mutex};
        EXPECT_GT(m.total_events, 0U);
        finished.push_back(run_id);
    };
    auto service = make(config);
    service->start();

    auto live   = start("te.live-a", "c1");
    live.action = ctl::CommandAction::StartLive;
    live.settings->live_feed = "test";
    send(live);
    auto second   = start("te.live-b", "c2");
    second.action = ctl::CommandAction::StartLive;
    send(second);
    const auto refused = answer("te.live-b", "c2");
    ASSERT_TRUE(refused.has_value());
    EXPECT_EQ(refused->error, "A live run is already running on this engine; stop it first.");

    send(start("te.bt-beside", "c3"));   // the backtest queue is not blocked by it
    ASSERT_TRUE(answer("te.bt-beside", "c3").has_value());
    for (int i = 0; i < 400 && reports("te.bt-beside").back().status != ctl::RunState::Completed; ++i) {
        std::this_thread::sleep_for(milliseconds{10});
    }
    EXPECT_EQ(reports("te.bt-beside").back().status, ctl::RunState::Completed);

    for (int i = 0; i < 400 && broker_->messages("te.live-a.market_data").size() < 40; ++i) {
        std::this_thread::sleep_for(milliseconds{10});
    }
    EXPECT_EQ(broker_->messages("te.live-a.market_data").size(), 40U);
    EXPECT_EQ(asked, (std::vector<tec::Symbol>{"AAPL", "MSFT"}));
    const auto running = reports("te.live-a").back();
    EXPECT_EQ(running.status, ctl::RunState::Running);
    EXPECT_FALSE(running.progress.has_value());
    EXPECT_EQ(running.settings.mode, trading_engine::config::RunMode::Live);
    EXPECT_TRUE(running.settings.period_start.has_value());
    EXPECT_FALSE(running.settings.period_end.has_value());
    const auto a = service->announcement();
    EXPECT_NE(std::find(a.runs.begin(), a.runs.end(), "te.live-a"), a.runs.end());

    ctl::Command stop;
    stop.command_id = "c4";
    stop.action     = ctl::CommandAction::StopRun;
    stop.run_id     = "te.live-a";
    send(stop);
    const auto stopped = answer("te.live-a", "c4");
    ASSERT_TRUE(stopped.has_value());
    EXPECT_EQ(stopped->status, ctl::RunState::Stopped);
    ASSERT_TRUE(service->wait_until_idle(seconds{10}));
    const std::lock_guard lock{finished_mutex};
    EXPECT_EQ(finished, (std::vector<std::string>{"te.bt-beside", "te.live-a"}));
}

TEST_F(EngineServiceTest, ALiveFeedThatGivesUpFailsTheRun) {
    app::EngineServiceConfig config;
    config.live_feed = [&](const std::vector<tec::Symbol>&, const std::string&, md::IMarketEventSink& sink,
                           const tec::IClock&) { return fake_feed(sink, bars(3), "auth failed"); };
    auto service = make(config);
    service->start();
    auto live   = start("te.live-f", "c1");
    live.action = ctl::CommandAction::StartLive;
    send(live);
    ASSERT_TRUE(answer("te.live-f", "c1").has_value());
    std::this_thread::sleep_for(milliseconds{50});
    ASSERT_TRUE(service->wait_until_idle(seconds{10}));
    const auto last = reports("te.live-f").back();
    EXPECT_EQ(last.status, ctl::RunState::Failed);
    EXPECT_EQ(last.error, "auth failed");
}

}  // namespace
