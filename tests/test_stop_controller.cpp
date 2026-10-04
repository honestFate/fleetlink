#include "fleetlink/stop_controller.hpp"
#include <doctest/doctest.h>
#include <stdexcept>
using namespace fleetlink;
namespace {
Message command(NodeId sender, SequenceNumber version, bool halt)
{
    const MessagePayload payload =
        halt ? MessagePayload{Halt{version}} : MessagePayload{Resume{version}};
    return {{{sender, version}, std::nullopt, 0, version}, payload};
}
} // namespace
TEST_CASE("each origin clears only its own HALT and old controls never undo a new command")
{
    StopController control(1, {1, 2, ARM_NODE_ID}, 50);
    CHECK(control.onMessage(command(ARM_NODE_ID, 1, true)));
    CHECK(control.onMessage(command(2, 1, true)));
    CHECK(control.halted());
    control.onMessage(command(ARM_NODE_ID, 2, false));
    CHECK(control.halted());
    control.onMessage(command(2, 2, false));
    CHECK_FALSE(control.halted());
    control.onMessage(command(ARM_NODE_ID, 1, true));
    CHECK_FALSE(control.halted());
    control.onMessage(command(ARM_NODE_ID, 3, true));
    control.onMessage(command(ARM_NODE_ID, 2, false));
    CHECK(control.halted());
    CHECK_FALSE(control.onMessage(command(99, 100, false)));
    auto other = command(ARM_NODE_ID, 4, false);
    other.header.recipient = 2;
    CHECK_FALSE(control.onMessage(other));
    CHECK(control.halted());
}
TEST_CASE("controls retry until every distinct addressee acknowledges the current command")
{
    StopController control(1, {1, 2, ARM_NODE_ID}, 50);
    const auto first = command(1, control.nextSequence(), true);
    control.issue(first);
    CHECK(control.halted());
    CHECK_FALSE(control.retry(49));
    REQUIRE(control.retry(50));
    CHECK(control.retry(100)->header.id == first.header.id);
    const Message ack{{{2, 10}, NodeId{1}, 100, 10}, Ack{first.header.id}};
    control.onMessage(ack);
    control.onMessage(ack);
    CHECK(control.retry(150).has_value());
    control.onMessage({{{ARM_NODE_ID, 10}, NodeId{1}, 150, 10}, Ack{first.header.id}});
    CHECK_FALSE(control.retry(200));
    control.issue(command(1, control.nextSequence(), false));
    CHECK_FALSE(control.halted());
    control.onMessage(ack);
    CHECK(control.retry(250).has_value());
    CHECK_THROWS_AS(control.issue(command(2, 10, true)), std::invalid_argument);
}
