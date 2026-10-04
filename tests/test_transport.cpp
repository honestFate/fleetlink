#include "fleetlink/transport.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <vector>

using namespace fleetlink;

namespace {

Message request(MachineId sender, SequenceNumber sequence, Tick tick,
                std::optional<NodeId> recipient = std::nullopt)
{
    return {{{sender, sequence}, recipient, tick, 20},
            Request{{ResourceId::LOADING_BUFFER, sender, sequence}, 10}};
}

std::vector<MessageId> ids(const std::vector<Message>& messages)
{
    std::vector<MessageId> result;
    result.reserve(messages.size());
    for (const auto& message : messages) {
        result.push_back(message.header.id);
    }
    return result;
}

} // namespace

TEST_CASE("unicast delivers an unchanged message once and no earlier than the next tick")
{
    InMemoryTransport channel({1, 2, 3});
    Transport& transport = channel;
    const auto message = request(1, 4, 0, 2);
    transport.send(message);

    CHECK(transport.receive(2).empty());
    channel.advanceTo(0);
    CHECK(transport.receive(2).empty());
    channel.advanceTo(1);

    const auto inbox = transport.receive(2);
    REQUIRE(inbox.size() == 1);
    CHECK(inbox.front().header.id == message.header.id);
    CHECK(inbox.front().header.recipient == message.header.recipient);
    CHECK(inbox.front().header.sent_tick == 0);
    CHECK(inbox.front().header.clock == 20);
    REQUIRE(messageKind(inbox.front()) == MessageKind::REQUEST);
    const auto& payload = std::get<Request>(inbox.front().payload);
    CHECK(payload.id == std::get<Request>(message.payload).id);
    CHECK(payload.timestamp == 10);
    CHECK(transport.receive(1).empty());
    CHECK(transport.receive(3).empty());
    CHECK(transport.receive(2).empty());
    channel.advanceTo(2);
    CHECK(transport.receive(2).empty());
}

TEST_CASE("broadcast delivers independent copies to all other nodes including ARM")
{
    InMemoryTransport channel({ARM_NODE_ID, 7, 0, 3});
    auto message = request(3, 1, 0);
    channel.send(message);
    std::get<Request>(message.payload).timestamp = 99;
    channel.advanceTo(1);

    for (const auto recipient : std::vector<NodeId>{0, 7, ARM_NODE_ID}) {
        auto inbox = channel.receive(recipient);
        REQUIRE(inbox.size() == 1);
        CHECK_FALSE(inbox.front().header.recipient.has_value());
        auto& payload = std::get<Request>(inbox.front().payload);
        CHECK(payload.timestamp == 10);
        payload.timestamp = 100;
        CHECK(channel.receive(recipient).empty());
    }
    CHECK(channel.receive(3).empty());
}

TEST_CASE("ARM missions and self-addressed messages use the same delivery queue")
{
    InMemoryTransport channel({ARM_NODE_ID, 0});
    const Message mission{{{ARM_NODE_ID, 1}, NodeId{0}, 0, 1}, Mission{1, MissionTarget::LOAD}};
    channel.send(mission);
    channel.send(request(0, 1, 0, 0));
    CHECK(channel.receive(0).empty());
    channel.advanceTo(1);

    const auto inbox = channel.receive(0);
    REQUIRE(inbox.size() == 2);
    CHECK(messageKind(inbox[0]) == MessageKind::REQUEST);
    REQUIRE(messageKind(inbox[1]) == MessageKind::MISSION);
    CHECK(std::get<Mission>(inbox[1].payload).first_target == MissionTarget::LOAD);
    CHECK(channel.receive(ARM_NODE_ID).empty());
}

TEST_CASE("a reply sent during message processing cannot arrive within the same tick")
{
    InMemoryTransport channel({1, 2});
    channel.send(request(1, 1, 0, 2));
    channel.advanceTo(1);
    const auto incoming = channel.receive(2);
    REQUIRE(incoming.size() == 1);

    const auto attempt = std::get<Request>(incoming.front().payload).id;
    channel.send({{{2, 1}, NodeId{1}, 1, 21}, Reply{attempt}});
    CHECK(channel.receive(1).empty());
    channel.advanceTo(1);
    CHECK(channel.receive(1).empty());
    channel.advanceTo(2);
    const auto replies = channel.receive(1);
    REQUIRE(replies.size() == 1);
    REQUIRE(messageKind(replies.front()) == MessageKind::REPLY);
    CHECK(std::get<Reply>(replies.front().payload).request == attempt);
}

TEST_CASE("delivery order is independent of registration and send order")
{
    const std::vector<Message> messages{request(3, 2, 0, 1), request(2, 1, 0), request(3, 1, 0, 1)};
    const std::vector<MessageId> expected{{2, 1}, {3, 1}, {3, 2}};

    for (const auto& nodes :
         std::vector<std::vector<NodeId>>{{1, 2, 3, ARM_NODE_ID}, {ARM_NODE_ID, 3, 2, 1}}) {
        std::vector<std::size_t> order{0, 1, 2};
        do {
            InMemoryTransport channel(nodes);
            for (const auto index : order) {
                channel.send(messages[index]);
            }
            channel.advanceTo(1);
            CHECK(ids(channel.receive(1)) == expected);
            CHECK(channel.receive(2).empty());
            CHECK(ids(channel.receive(3)) == std::vector<MessageId>{{2, 1}});
            CHECK(ids(channel.receive(ARM_NODE_ID)) == std::vector<MessageId>{{2, 1}});
        } while (std::next_permutation(order.begin(), order.end()));
    }
}

TEST_CASE("unread messages survive advancing time and precede later arrivals")
{
    InMemoryTransport channel({1, 2});
    channel.send(request(1, 1, 0, 2));
    channel.advanceTo(1);
    channel.send(request(1, 2, 1, 2));
    channel.advanceTo(5);

    const auto inbox = channel.receive(2);
    REQUIRE(inbox.size() == 2);
    CHECK(ids(inbox) == std::vector<MessageId>{{1, 1}, {1, 2}});
    CHECK(inbox[0].header.sent_tick == 0);
    CHECK(inbox[1].header.sent_tick == 1);
    CHECK(channel.receive(2).empty());
}

TEST_CASE("invalid node configurations and addresses are rejected")
{
    CHECK_THROWS_AS(InMemoryTransport(std::vector<NodeId>{}), std::invalid_argument);
    CHECK_THROWS_AS(InMemoryTransport(std::vector<NodeId>{1, 2, 1}), std::invalid_argument);

    InMemoryTransport channel({1, 2});
    CHECK_THROWS_AS(channel.send(request(3, 1, 0)), std::invalid_argument);
    CHECK_THROWS_AS(channel.send(request(1, 1, 0, 3)), std::invalid_argument);
    CHECK_THROWS_AS(channel.receive(3), std::invalid_argument);
    channel.send(request(1, 2, 0, 2));
    channel.advanceTo(1);
    CHECK(ids(channel.receive(2)) == std::vector<MessageId>{{1, 2}});

    InMemoryTransport alone({1});
    alone.send(request(1, 1, 0));
    alone.advanceTo(1);
    CHECK(alone.receive(1).empty());
}

TEST_CASE("invalid time changes leave queued messages intact")
{
    InMemoryTransport channel({1, 2});
    CHECK_THROWS_AS(channel.send(request(1, 1, 1, 2)), std::invalid_argument);
    channel.send(request(1, 2, 0, 2));
    channel.advanceTo(1);
    CHECK_THROWS_AS(channel.send(request(1, 3, 0, 2)), std::invalid_argument);
    channel.send(request(1, 4, 1, 2));
    CHECK_THROWS_AS(channel.advanceTo(0), std::invalid_argument);
    CHECK(ids(channel.receive(2)) == std::vector<MessageId>{{1, 2}});
    channel.advanceTo(2);
    CHECK(ids(channel.receive(2)) == std::vector<MessageId>{{1, 4}});
}

TEST_CASE("transport reports tick overflow instead of wrapping the delivery time")
{
    InMemoryTransport channel({1, 2});
    const auto last = std::numeric_limits<Tick>::max();
    channel.advanceTo(last - 1);
    channel.send(request(1, 1, last - 1, 2));
    channel.advanceTo(last);
    CHECK(ids(channel.receive(2)) == std::vector<MessageId>{{1, 1}});
    CHECK_THROWS_AS(channel.send(request(1, 2, last, 2)), std::overflow_error);
    channel.advanceTo(last);
    CHECK(channel.receive(2).empty());
}
