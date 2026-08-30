#include "rt/ByteFifo.hpp"
#include "rt/RcuSlot.hpp"
#include "rt/SmoothedParam.hpp"
#include "rt/SpscRing.hpp"
#include "rt/TimelineFifo.hpp"
#include "rt/ValueSlot.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <thread>
#include <vector>

namespace {

using avc::rt::RcuSlot;
using avc::rt::SmoothedParam;
using avc::rt::SpscRing;

TEST(SpscRing, PopsInPushOrder)
{
    SpscRing<int, 8> ring;
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(ring.push(i));
    }
    for (int i = 0; i < 5; ++i) {
        int value = -1;
        ASSERT_TRUE(ring.pop(value));
        EXPECT_EQ(value, i);
    }
    int drained = 0;
    EXPECT_FALSE(ring.pop(drained));
}

TEST(SpscRing, RefusesToOverwriteWhenFull)
{
    SpscRing<int, 4> ring;
    EXPECT_EQ(ring.capacity(), 3U);
    EXPECT_TRUE(ring.push(1));
    EXPECT_TRUE(ring.push(2));
    EXPECT_TRUE(ring.push(3));
    EXPECT_FALSE(ring.push(4));

    int value = 0;
    ASSERT_TRUE(ring.pop(value));
    EXPECT_EQ(value, 1);
    EXPECT_TRUE(ring.push(4));
}

TEST(SpscRing, WrapsAroundRepeatedly)
{
    SpscRing<int, 4> ring;
    for (int round = 0; round < 1000; ++round) {
        ASSERT_TRUE(ring.push(round));
        int value = -1;
        ASSERT_TRUE(ring.pop(value));
        EXPECT_EQ(value, round);
    }
    EXPECT_TRUE(ring.empty());
}

TEST(SpscRing, SurvivesConcurrentProducerAndConsumer)
{
    constexpr int kCount = 200000;
    SpscRing<int, 64> ring;
    std::atomic<bool> failed{false};

    std::thread consumer([&] {
        int expected = 0;
        while (expected < kCount) {
            int value = -1;
            if (!ring.pop(value)) {
                continue;
            }
            if (value != expected) {
                failed.store(true);
                return;
            }
            ++expected;
        }
    });

    for (int i = 0; i < kCount; ++i) {
        while (!ring.push(i)) {
        }
    }
    consumer.join();
    EXPECT_FALSE(failed.load());
}

TEST(SmoothedParam, ApproachesTargetWithoutJumping)
{
    SmoothedParam param;
    param.configure(0.2F);
    param.snap(0.0F);
    param.setTarget(1.0F);

    const float first = param.nextBlock();
    EXPECT_GT(first, 0.0F);
    EXPECT_LT(first, 1.0F);

    for (int i = 0; i < 200; ++i) {
        param.nextBlock();
    }
    EXPECT_TRUE(param.settled());
    EXPECT_NEAR(param.current(), 1.0F, 1e-4F);
}

TEST(SmoothedParam, SnapBypassesSmoothing)
{
    SmoothedParam param;
    param.snap(0.5F);
    EXPECT_FLOAT_EQ(param.current(), 0.5F);
    EXPECT_FLOAT_EQ(param.nextBlock(), 0.5F);
}

TEST(RcuSlot, GuardSeesTheInstalledObject)
{
    RcuSlot<int> slot;
    {
        auto guard = slot.enter();
        EXPECT_EQ(guard.get(), nullptr);
    }

    slot.store(std::make_unique<int>(42));
    {
        auto guard = slot.enter();
        ASSERT_NE(guard.get(), nullptr);
        EXPECT_EQ(*guard.get(), 42);
    }
}

TEST(RcuSlot, CollectDefersWhileAGuardIsOpen)
{
    RcuSlot<int> slot;
    slot.store(std::make_unique<int>(1));

    auto guard = std::make_unique<RcuSlot<int>::Guard>(slot);
    EXPECT_EQ(*guard->get(), 1);

    slot.store(std::make_unique<int>(2));
    EXPECT_EQ(slot.pendingRetired(), 1U);

    EXPECT_EQ(slot.collect(), 0U);
    EXPECT_EQ(*guard->get(), 1);

    guard.reset();
    EXPECT_EQ(slot.collect(), 1U);
    EXPECT_EQ(slot.pendingRetired(), 0U);
}

using avc::rt::ByteFifo;

constexpr std::uint32_t bytesOf(std::uint32_t frames)
{
    return frames * static_cast<std::uint32_t>(sizeof(float));
}

std::byte *raw(std::vector<float> &samples)
{
    return reinterpret_cast<std::byte *>(samples.data());
}

const std::byte *raw(const std::vector<float> &samples)
{
    return reinterpret_cast<const std::byte *>(samples.data());
}

TEST(ByteFifo, RoundsCapacityUpToAPowerOfTwo)
{
    ByteFifo fifo;
    fifo.reset(300);
    EXPECT_EQ(fifo.capacity(), 512U);
    EXPECT_EQ(fifo.readable(), 0U);
    EXPECT_EQ(fifo.writable(), 512U);
}

TEST(ByteFifo, CarriesSamplesThrough)
{
    ByteFifo fifo;
    fifo.reset(bytesOf(64));

    const std::vector<float> in{1.0F, 2.0F, 3.0F, 4.0F};
    ASSERT_TRUE(fifo.write(raw(in), bytesOf(4)));
    EXPECT_EQ(fifo.readable(), bytesOf(4));

    std::vector<float> out(4, 0.0F);
    ASSERT_TRUE(fifo.read(raw(out), bytesOf(4)));
    EXPECT_EQ(out, in);
    EXPECT_EQ(fifo.readable(), 0U);
}

TEST(ByteFifo, ReadsAndWritesAreAllOrNothing)
{
    ByteFifo fifo;
    fifo.reset(bytesOf(8));

    const std::vector<float> in(4, 1.0F);
    ASSERT_TRUE(fifo.write(raw(in), bytesOf(4)));

    std::vector<float> out(8, -1.0F);
    EXPECT_FALSE(fifo.read(raw(out), bytesOf(8)));
    EXPECT_EQ(out[0], -1.0F) << "a refused read must not touch the destination";
    EXPECT_EQ(fifo.readable(), bytesOf(4));

    const std::vector<float> too_much(8, 2.0F);
    EXPECT_FALSE(fifo.write(raw(too_much), bytesOf(8)));
    EXPECT_EQ(fifo.readable(), bytesOf(4));
}

TEST(ByteFifo, WrapsWithoutLosingASample)
{
    ByteFifo fifo;
    fifo.reset(bytesOf(8));

    float next = 0.0F;
    float expected = 0.0F;
    for (int round = 0; round < 40; ++round) {
        std::vector<float> in(3);
        for (float &sample : in) {
            sample = next++;
        }
        ASSERT_TRUE(fifo.write(raw(in), bytesOf(3))) << "round " << round;

        std::vector<float> out(3, -1.0F);
        ASSERT_TRUE(fifo.read(raw(out), bytesOf(3))) << "round " << round;
        for (float sample : out) {
            EXPECT_EQ(sample, expected++);
        }
    }
}

TEST(ByteFifo, WrapsOnAByteBoundaryWhateverThePayloadIs)
{
    ByteFifo fifo;
    fifo.reset(16);

    for (int round = 0; round < 40; ++round) {
        const std::array<std::byte, 5> in{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
                                          static_cast<std::byte>(round)};
        ASSERT_TRUE(fifo.write(in.data(), 5)) << "round " << round;

        std::array<std::byte, 5> out{};
        ASSERT_TRUE(fifo.read(out.data(), 5)) << "round " << round;
        EXPECT_EQ(out, in);
    }
}

TEST(ByteFifo, PrefillIsReadableSilence)
{
    ByteFifo fifo;
    fifo.reset(bytesOf(64));
    fifo.prefill(bytesOf(16));
    EXPECT_EQ(fifo.readable(), bytesOf(16));

    std::vector<float> out(16, -1.0F);
    ASSERT_TRUE(fifo.read(raw(out), bytesOf(16)));
    EXPECT_EQ(out, std::vector<float>(16, 0.0F));
}

TEST(ByteFifo, SurvivesAProducerAndAConsumerOnTwoThreads)
{
    ByteFifo fifo;
    fifo.reset(bytesOf(256));
    constexpr int kBlocks = 20000;
    constexpr std::uint32_t kBlock = 32;

    std::thread producer([&] {
        float next = 0.0F;
        for (int i = 0; i < kBlocks;) {
            std::vector<float> block(kBlock);
            for (float &sample : block) {
                sample = next + static_cast<float>(&sample - block.data());
            }
            if (fifo.write(raw(block), bytesOf(kBlock))) {
                next += kBlock;
                ++i;
            }
        }
    });

    float expected = 0.0F;
    for (int i = 0; i < kBlocks;) {
        std::vector<float> block(kBlock, -1.0F);
        if (!fifo.read(raw(block), bytesOf(kBlock))) {
            continue;
        }
        for (float sample : block) {
            ASSERT_EQ(sample, expected++);
        }
        ++i;
    }
    producer.join();
}

using avc::rt::TimelineFifo;

TEST(TimelineFifo, ReadsOnlyTheRequestedAbsoluteInterval)
{
    TimelineFifo fifo;
    fifo.reset(16, sizeof(float));
    const std::vector<float> first{1.0F, 2.0F, 3.0F, 4.0F};
    ASSERT_TRUE(fifo.write(raw(first), 100, 4));

    std::vector<float> out(4, 0.0F);
    const auto read = fifo.read(raw(out), 100, 4);
    EXPECT_TRUE(read.exact);
    EXPECT_FALSE(read.discontinuity);
    EXPECT_EQ(out, first);
}

TEST(TimelineFifo, RefusesAResultAfterItsPlaybackDeadline)
{
    TimelineFifo fifo;
    fifo.reset(16, sizeof(float));

    std::vector<float> out(4, -1.0F);
    const auto missing = fifo.read(raw(out), 0, 4);
    EXPECT_FALSE(missing.exact);
    EXPECT_TRUE(missing.discontinuity);
    EXPECT_EQ(out, std::vector<float>(4, 0.0F));

    const std::vector<float> late(4, 9.0F);
    EXPECT_FALSE(fifo.write(raw(late), 0, 4));
    EXPECT_EQ(fifo.requestedUntil(), 4U);
}

TEST(TimelineFifo, ResolvesDroppedPayloadAsAGapAndRecoversAtTheCurrentFrame)
{
    TimelineFifo fifo;
    fifo.reset(4, sizeof(float));
    const std::vector<float> stale(4, 1.0F);
    const std::vector<float> dropped(4, 2.0F);
    ASSERT_TRUE(fifo.write(raw(stale), 0, 4));
    EXPECT_FALSE(fifo.write(raw(dropped), 4, 4)) << "the ring is full";
    EXPECT_TRUE(fifo.canResolve(4, 4)) << "the producer still published the gap";

    std::vector<float> out(4, -1.0F);
    const auto gap = fifo.read(raw(out), 4, 4);
    EXPECT_FALSE(gap.exact);
    EXPECT_TRUE(gap.discontinuity);
    EXPECT_EQ(out, std::vector<float>(4, 0.0F));

    const std::vector<float> current(4, 3.0F);
    ASSERT_TRUE(fifo.write(raw(current), 8, 4));
    const auto recovered = fifo.read(raw(out), 8, 4);
    EXPECT_TRUE(recovered.exact);
    EXPECT_TRUE(std::equal(out.begin(), out.end(), current.begin()));
}

TEST(TimelineFifo, PrefillHasARealTimelineAndDoesNotCountAsAGap)
{
    TimelineFifo fifo;
    fifo.reset(16, sizeof(float));
    fifo.prefill(0, 8);

    std::vector<float> out(4, -1.0F);
    EXPECT_TRUE(fifo.read(raw(out), 0, 4).exact);
    EXPECT_EQ(out, std::vector<float>(4, 0.0F));
    EXPECT_TRUE(fifo.read(raw(out), 4, 4).exact);
}

TEST(TimelineFifo, ExplicitProducerAndConsumerGapsAdvanceTheDeadline)
{
    TimelineFifo fifo;
    fifo.reset(16, sizeof(float));
    fifo.publishGap(0, 8);
    EXPECT_TRUE(fifo.canResolve(0, 8));

    fifo.expire(0, 8);
    const std::vector<float> late(8, 1.0F);
    EXPECT_FALSE(fifo.write(raw(late), 0, 8));
    EXPECT_EQ(fifo.requestedUntil(), 8U);
}

TEST(TimelineFifo, CarriesAnExplicitDiscontinuityWithTheExactPayload)
{
    TimelineFifo fifo;
    fifo.reset(16, sizeof(float));
    const std::vector<float> input(4, 0.25F);
    ASSERT_TRUE(fifo.write(raw(input), 32, 4, true));

    std::vector<float> output(4, 0.0F);
    const auto read = fifo.read(raw(output), 32, 4);
    EXPECT_TRUE(read.exact);
    EXPECT_TRUE(read.discontinuity);
    EXPECT_EQ(output, input);
}

using avc::rt::ValueSlot;

std::array<std::byte, 8> valueOf(std::uint32_t mark)
{
    std::array<std::byte, 8> block{};
    std::memcpy(block.data(), &mark, sizeof(mark));
    return block;
}

TEST(ValueSlot, HandsOverTheNewestAndSaysWhenThereIsNothingNew)
{
    ValueSlot slot;
    slot.reset(8);

    std::array<std::byte, 8> out{};
    std::memset(out.data(), 0xEE, out.size());
    EXPECT_FALSE(slot.read(out.data())) << "nothing has been published yet";
    EXPECT_EQ(out[0], std::byte{0xEE}) << "a read with nothing new must not touch the destination";

    slot.write(valueOf(7).data());
    ASSERT_TRUE(slot.read(out.data()));
    EXPECT_EQ(out, valueOf(7));

    EXPECT_FALSE(slot.read(out.data()));
    EXPECT_EQ(out, valueOf(7)) << "the consumer keeps the value it already has";
}

TEST(ValueSlot, KeepsOnlyTheLatest)
{
    ValueSlot slot;
    slot.reset(8);

    for (std::uint32_t i = 1; i <= 100; ++i) {
        slot.write(valueOf(i).data());
    }

    std::array<std::byte, 8> out{};
    ASSERT_TRUE(slot.read(out.data()));
    EXPECT_EQ(out, valueOf(100));
    EXPECT_FALSE(slot.read(out.data()));
}

TEST(ValueSlot, NeverTearsAcrossTwoThreads)
{
    ValueSlot slot;
    constexpr std::uint32_t kBytes = 256;
    slot.reset(kBytes);

    std::atomic<bool> stop{false};
    std::thread producer([&] {
        for (std::uint32_t mark = 1; !stop.load(std::memory_order_relaxed); ++mark) {
            std::vector<std::byte> block(kBytes, static_cast<std::byte>(mark & 0xFFU));
            slot.write(block.data());
        }
    });

    std::vector<std::byte> out(kBytes, std::byte{0});
    for (int taken = 0; taken < 5000;) {
        if (!slot.read(out.data())) {
            continue;
        }
        for (std::byte value : out) {
            ASSERT_EQ(value, out[0]) << "a block was half one value and half another";
        }
        ++taken;
    }
    stop.store(true, std::memory_order_relaxed);
    producer.join();
}

}
