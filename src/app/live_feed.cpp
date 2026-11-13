#include "trading_engine/app/live_feed.hpp"

#include "trading_engine/app/configured_run.hpp"

namespace trading_engine::app {

void LiveEventQueue::on_market_event(const domain::MarketEvent& event) {
    {
        const std::lock_guard lock{mutex_};
        queue_.push_back(event);
    }
    ready_.notify_one();
}

std::optional<domain::MarketEvent> LiveEventQueue::pop(std::chrono::milliseconds timeout) {
    std::unique_lock lock{mutex_};
    if (!ready_.wait_for(lock, timeout, [this] { return !queue_.empty(); })) {
        return std::nullopt;
    }
    domain::MarketEvent event = std::move(queue_.front());
    queue_.pop_front();
    return event;
}

std::optional<std::string> feed_live(ConfiguredRun& run, LiveFeed& feed, LiveEventQueue& queue,
                                     common::Timestamp start, const LiveHooks& hooks) {
    const auto one = [&](const domain::MarketEvent& event) {
        if (hooks.before_feed) {
            hooks.before_feed(event);
        }
        run.feed(event);
        if (hooks.after_feed) {
            hooks.after_feed(event);
        }
    };

    feed.source->start();
    run.begin(start);
    while (!hooks.keep_going || hooks.keep_going()) {
        if (const auto event = queue.pop(std::chrono::milliseconds{250})) {
            one(*event);
        } else if (!feed.source->is_running()) {
            break;
        }
    }
    feed.source->stop();
    while (const auto event = queue.pop(std::chrono::milliseconds{0})) {
        one(*event);
    }
    run.end();
    return feed.failure ? feed.failure() : std::nullopt;
}

}  // namespace trading_engine::app
