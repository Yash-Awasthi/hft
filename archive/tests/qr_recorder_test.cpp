// Moved out of tests/queue_reactive_test.cpp.
// The calibration recorder sees exactly the simulator's events: every Gillespie event once,
// redraws only at the instant of a reference move, and one depletion episode per move.
TEST(QueueReactive, RecorderSeesTheSimulatedEventsAndMoves) {
    const auto p = params();
    sources::QueueReactive sim(p, 9);
    std::vector<std::uint8_t> raw;
    sim.day(raw);
    strategy::QrRecorder rec(3, 100, p.start_ns, p.end_ns);
    itch::Frame f{};
    for (std::size_t pos = 0, k; (k = itch::next_frame(raw.data() + pos, raw.size() - pos, f)); pos += k)
        if (f.data[0] != 'S' && f.data[0] != 'R') rec.on_itch(f.data, f.size);
    const auto& r = rec.record();
    std::uint64_t regular = 0;
    for (std::size_t i = 0; i < r.ts.size(); ++i) {
        regular += !r.after_move[i];
        EXPECT_EQ(r.shares[i], r.kind[i] == 3 ? 0u : 100u);
    }
    EXPECT_EQ(regular, sim.events());
    EXPECT_EQ(r.moves_ts.size(), sim.moves());
    EXPECT_EQ(r.episodes_moved, sim.moves());
    EXPECT_GT(r.episodes_refilled, 0u);
}
