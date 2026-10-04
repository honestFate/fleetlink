#include "fleetlink/messages.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace fleetlink;

namespace {

Message heartbeat(NodeId sender, SequenceNumber sequence, Tick tick,
                  std::optional<NodeId> recipient = std::nullopt)
{
    return {{{sender, sequence}, recipient, tick, 1}, Heartbeat{sequence}};
}

} // namespace

TEST_CASE("unicast addresses one node including ARM and machine zero")
{
    const std::vector<NodeId> nodes{7, ARM_NODE_ID, 0, 3};

    CHECK(recipientsFor(heartbeat(7, 1, 0, 3), nodes) == std::vector<NodeId>{3});
    CHECK(recipientsFor(heartbeat(7, 1, 0, ARM_NODE_ID), nodes) ==
          std::vector<NodeId>{ARM_NODE_ID});
    CHECK(recipientsFor(heartbeat(ARM_NODE_ID, 1, 0, 0), nodes) == std::vector<NodeId>{0});
    CHECK(recipientsFor(heartbeat(7, 1, 0, 7), nodes) == std::vector<NodeId>{7});
}

TEST_CASE("broadcast excludes its sender and is independent of the node list order")
{
    std::vector<NodeId> nodes{0, 3, 7, ARM_NODE_ID};
    const std::vector<NodeId> expected{0, 7, ARM_NODE_ID};

    do {
        // Keep a snapshot to detect changes to the input
        // NOLINTNEXTLINE(performance-unnecessary-copy-initialization)
        const auto original = nodes;
        CHECK(recipientsFor(heartbeat(3, 1, 0), nodes) == expected);
        CHECK(nodes == original);
    } while (std::next_permutation(nodes.begin(), nodes.end()));

    CHECK(recipientsFor(heartbeat(3, 1, 0), {3}).empty());
}

TEST_CASE("invalid addressing is rejected before delivery")
{
    CHECK_THROWS_AS(recipientsFor(heartbeat(1, 1, 0), {1, 2, 2}), std::invalid_argument);
    CHECK_THROWS_AS(recipientsFor(heartbeat(3, 1, 0), {1, 2}), std::invalid_argument);
    CHECK_THROWS_AS(recipientsFor(heartbeat(1, 1, 0, 3), {1, 2}), std::invalid_argument);
    CHECK_THROWS_AS(recipientsFor(heartbeat(1, 1, 0), {}), std::invalid_argument);
}

TEST_CASE("message order depends on send tick and stable ids instead of insertion order")
{
    const std::vector<Message> input{
        heartbeat(2, 1, 10),
        heartbeat(1, 2, 10),
        heartbeat(3, 1, 5),
        heartbeat(1, 1, 10),
    };
    const std::vector<MessageId> expected{{3, 1}, {1, 1}, {1, 2}, {2, 1}};
    std::vector<int> permutation{0, 1, 2, 3};

    do {
        std::vector<Message> messages;
        messages.reserve(input.size());
        for (const auto index : permutation) {
            messages.push_back(input[index]);
        }
        std::sort(messages.begin(), messages.end(), messageBefore);
        std::vector<MessageId> actual;
        actual.reserve(messages.size());
        for (const auto& message : messages) {
            actual.push_back(message.header.id);
        }
        CHECK(actual == expected);
    } while (std::next_permutation(permutation.begin(), permutation.end()));
}

TEST_CASE("zero delay delivers on the next tick and positive delay keeps its meaning")
{
    CHECK(deliveryTick(10, 0) == 11);
    CHECK(deliveryTick(10, 1) == 11);
    CHECK(deliveryTick(10, 5) == 15);
}

TEST_CASE("delivery time overflow is reported instead of wrapping into the past")
{
    const auto last = std::numeric_limits<Tick>::max();
    CHECK(deliveryTick(last - 1, 0) == last);
    CHECK(deliveryTick(0, last) == last);
    CHECK_THROWS_AS(deliveryTick(last, 0), std::overflow_error);
    CHECK_THROWS_AS(deliveryTick(last - 1, 2), std::overflow_error);
}

TEST_CASE("request ordering uses logical time then stable machine id")
{
    std::vector<RequestKey> keys{{10, 7}, {11, 0}, {9, 9}, {10, 3}};
    const std::vector<RequestKey> expected{{9, 9}, {10, 3}, {10, 7}, {11, 0}};
    std::sort(keys.begin(), keys.end());
    CHECK(keys == expected);
    CHECK_FALSE(keys.front() < keys.front());
}

TEST_CASE("a retransmitted request retains priority despite a later message clock")
{
    const Request attempt{{ResourceId::LOADING_BUFFER, 3, 1}, 10};
    const Message first{{{3, 4}, std::nullopt, 20, 10}, attempt};
    const Message retry{{{3, 9}, std::nullopt, 45, 30}, attempt};
    const Request competitor{{ResourceId::LOADING_BUFFER, 7, 1}, 11};

    const auto& original = std::get<Request>(first.payload);
    const auto& repeated = std::get<Request>(retry.payload);
    CHECK(requestKey(original) == requestKey(repeated));
    CHECK(requestKey(repeated) < requestKey(competitor));
    CHECK(original.id == repeated.id);
    CHECK_FALSE(first.header.id == retry.header.id);
    CHECK(messageKind(retry) == MessageKind::REQUEST);
}

TEST_CASE("Lamport time advances on sends and follows received logical time")
{
    CHECK(nextLamportTime(0) == 1);
    CHECK(nextLamportTime(10, 3) == 11);
    CHECK(nextLamportTime(3, 10) == 11);
    CHECK(nextLamportTime(10, 10) == 11);

    const auto last = std::numeric_limits<LamportTime>::max();
    CHECK(nextLamportTime(last - 1) == last);
    CHECK_THROWS_AS(nextLamportTime(last), std::overflow_error);
    CHECK_THROWS_AS(nextLamportTime(0, last), std::overflow_error);
}
