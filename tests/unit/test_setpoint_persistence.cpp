/// test_setpoint_persistence.cpp — Regression tests for HEAT_COOL band
/// persistence across reboots (opt-in `supports.restore_setpoints`).
///
/// Bug: the dual-setpoint band is synthetic — a Mitsubishi unit stores only a
/// single setpoint and runs HEAT_COOL as hardware AUTO, so the user's low/high
/// band lives only in RAM. On reboot, setup() NANs target_temperature_low/high
/// (componentEntries.cpp) and the unit's hardware-AUTO report drives the entity
/// back to CLIMATE_MODE_AUTO (hp_readings.cpp), losing both the HEAT_COOL mode
/// and the band.
///
/// Fix (opt-in): persist {version, mode, low, high} to flash on every applied
/// control, and in setup() re-seed HEAT_COOL + the band before the first
/// settings read. The existing AUTO-branch guard in checkPowerAndModeSettings()
/// then keeps HEAT_COOL while hardware reports HEAT, COOL or AUTO; OFF
/// and the other supported mode transitions remain available.
///
/// Uses the production persisted record and restore decision; flash I/O stays in the profile.
#include <gtest/gtest.h>
#include "heatpump_profile_protocol.h"

using esphome::cn105::HeatPumpProtocol;
using SetpointState = HeatPumpProtocol::SetpointState;
using namespace esphome::climate;
constexpr uint8_t kStateVersion = 1;
static_assert(sizeof(SetpointState) == 10, "Preserve the existing packed flash record");

// ── Happy path: a saved HEAT_COOL band is restored exactly ──

TEST(SetpointPersistenceTest, RestoresHeatCoolBand) {
    auto s = HeatPumpProtocol::saved_setpoint_state(CLIMATE_MODE_HEAT_COOL, 20.0f, 24.0f);
    auto r = HeatPumpProtocol::restored_setpoint_state(/*restoreEnabled=*/true, /*loaded=*/true, s, /*dual=*/true);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->mode, static_cast<uint8_t>(CLIMATE_MODE_HEAT_COOL));
    EXPECT_FLOAT_EQ(r->target_low, 20.0f);
    EXPECT_FLOAT_EQ(r->target_high, 24.0f);
}

TEST(SetpointPersistenceTest, SaveRestoreRoundTripIsLossless) {
    auto s = HeatPumpProtocol::saved_setpoint_state(CLIMATE_MODE_HEAT_COOL, 18.5f, 23.5f);
    auto r = HeatPumpProtocol::restored_setpoint_state(true, true, s, true);
    ASSERT_TRUE(r.has_value());
    EXPECT_FLOAT_EQ(r->target_low, 18.5f);
    EXPECT_FLOAT_EQ(r->target_high, 23.5f);
}

// ── Opt-in: nothing happens unless the feature is enabled ──

TEST(SetpointPersistenceTest, DisabledDoesNotRestore) {
    auto s = HeatPumpProtocol::saved_setpoint_state(CLIMATE_MODE_HEAT_COOL, 20.0f, 24.0f);
    EXPECT_FALSE(HeatPumpProtocol::restored_setpoint_state(/*restoreEnabled=*/false, true, s, true).has_value());
}

TEST(SetpointPersistenceTest, NoSavedStateDoesNotRestore) {
    SetpointState empty{};  // load() failed
    EXPECT_FALSE(HeatPumpProtocol::restored_setpoint_state(true, /*loaded=*/false, empty, true).has_value());
}

// ── Guards: never restore a stale / non-HEAT_COOL / invalid record ──

TEST(SetpointPersistenceTest, VersionMismatchDoesNotRestore) {
    SetpointState s{kStateVersion + 1u, static_cast<uint8_t>(CLIMATE_MODE_HEAT_COOL), 20.0f, 24.0f};
    EXPECT_FALSE(HeatPumpProtocol::restored_setpoint_state(true, true, s, true).has_value());
}

TEST(SetpointPersistenceTest, SingleModeDoesNotRestore) {
    // Single modes (HEAT/COOL) are persisted by the heat pump itself, so we
    // must not force HEAT_COOL when the last saved mode was single.
    auto s = HeatPumpProtocol::saved_setpoint_state(CLIMATE_MODE_COOL, 22.0f, 22.0f);
    EXPECT_FALSE(HeatPumpProtocol::restored_setpoint_state(true, true, s, true).has_value());
}

TEST(SetpointPersistenceTest, NanBandDoesNotRestore) {
    auto s = HeatPumpProtocol::saved_setpoint_state(CLIMATE_MODE_HEAT_COOL, NAN, 24.0f);
    EXPECT_FALSE(HeatPumpProtocol::restored_setpoint_state(true, true, s, true).has_value());
}

TEST(SetpointPersistenceTest, DualNotSupportedDoesNotRestore) {
    auto s = HeatPumpProtocol::saved_setpoint_state(CLIMATE_MODE_HEAT_COOL, 20.0f, 24.0f);
    EXPECT_FALSE(HeatPumpProtocol::restored_setpoint_state(true, true, s, /*dual=*/false).has_value());
}

// ── Per-mode setpoint persistence (COOL / HEAT / AUTO) ──

using ModeSetpoints = HeatPumpProtocol::ModeSetpoints;
static_assert(sizeof(ModeSetpoints) == 4, "Keep ModeSetpoints in a single 4-byte preference slot");

TEST(ModeSetpointsTest, DefaultIsUnset) {
    ModeSetpoints ms{};
    EXPECT_FALSE(ms.get(CLIMATE_MODE_COOL).has_value());
    EXPECT_FALSE(ms.get(CLIMATE_MODE_HEAT).has_value());
    EXPECT_FALSE(ms.get(CLIMATE_MODE_AUTO).has_value());
    EXPECT_FALSE(ms.get(CLIMATE_MODE_DRY).has_value());
    EXPECT_FALSE(ms.get(CLIMATE_MODE_OFF).has_value());
}

TEST(ModeSetpointsTest, StoresAndRetrievesPerModeSetpoints) {
    ModeSetpoints ms{};
    EXPECT_TRUE(ms.update_from_mode_str("ON", "HEAT", 19.5f));
    EXPECT_TRUE(ms.update_from_mode_str("ON", "COOL", 24.0f));
    EXPECT_TRUE(ms.update_from_mode_str("ON", "AUTO", 21.5f));

    ASSERT_TRUE(ms.get(CLIMATE_MODE_HEAT).has_value());
    ASSERT_TRUE(ms.get(CLIMATE_MODE_COOL).has_value());
    ASSERT_TRUE(ms.get(CLIMATE_MODE_AUTO).has_value());
    EXPECT_FLOAT_EQ(*ms.get(CLIMATE_MODE_HEAT), 19.5f);
    EXPECT_FLOAT_EQ(*ms.get(CLIMATE_MODE_COOL), 24.0f);
    EXPECT_FLOAT_EQ(*ms.get(CLIMATE_MODE_AUTO), 21.5f);

    // Unchanged value returns false (avoids redundant flash writes).
    EXPECT_FALSE(ms.update_from_mode_str("ON", "HEAT", 19.5f));
    EXPECT_TRUE(ms.update_from_mode_str("ON", "HEAT", 20.0f));
    EXPECT_FLOAT_EQ(*ms.get(CLIMATE_MODE_HEAT), 20.0f);
}

TEST(ModeSetpointsTest, IgnoresOffDryFanAndOutOfRange) {
    ModeSetpoints ms{};
    EXPECT_FALSE(ms.update_from_mode_str("OFF", "HEAT", 20.0f));
    EXPECT_FALSE(ms.update_from_mode_str("ON", "DRY", 24.0f));
    EXPECT_FALSE(ms.update_from_mode_str("ON", "FAN", 22.0f));
    EXPECT_FALSE(ms.update_from_mode_str("ON", "COOL", NAN));
    EXPECT_FALSE(ms.update_from_mode_str("ON", "COOL", 9.5f));
    EXPECT_FALSE(ms.update_from_mode_str("ON", "COOL", 31.5f));
    EXPECT_FALSE(ms.get(CLIMATE_MODE_HEAT).has_value());
    EXPECT_FALSE(ms.get(CLIMATE_MODE_COOL).has_value());
}

TEST(ModeSetpointsTest, SupportsEncodingBLowerBound) {
    ModeSetpoints ms{};
    EXPECT_TRUE(ms.update_from_mode_str("ON", "HEAT", 10.0f));
    EXPECT_TRUE(ms.update_from_mode_str("ON", "COOL", 31.0f));
    EXPECT_FLOAT_EQ(*ms.get(CLIMATE_MODE_HEAT), 10.0f);
    EXPECT_FLOAT_EQ(*ms.get(CLIMATE_MODE_COOL), 31.0f);
}

TEST(ModeSetpointsTest, ValidatedRejectsBadVersionAndCorruptSlots) {
    ModeSetpoints bad_ver{};
    bad_ver.version = ModeSetpoints::kVersion + 1;
    bad_ver.heat_steps = ModeSetpoints::encode(20.0f);
    auto r1 = ModeSetpoints::validated(true, bad_ver);
    EXPECT_FALSE(r1.get(CLIMATE_MODE_HEAT).has_value());

    ModeSetpoints corrupt{};
    corrupt.heat_steps = ModeSetpoints::encode(19.5f);
    corrupt.cool_steps = 200;  // decodes to 110.0°C > kMaxTemp
    auto r2 = ModeSetpoints::validated(true, corrupt);
    ASSERT_TRUE(r2.get(CLIMATE_MODE_HEAT).has_value());
    EXPECT_FLOAT_EQ(*r2.get(CLIMATE_MODE_HEAT), 19.5f);
    EXPECT_FALSE(r2.get(CLIMATE_MODE_COOL).has_value());

    auto r3 = ModeSetpoints::validated(false, corrupt);
    EXPECT_FALSE(r3.get(CLIMATE_MODE_HEAT).has_value());
}

TEST(ModeSetpointsTest, DirectSetQuantizesAndRejectsInvalid) {
    ModeSetpoints ms{};
    auto q_heat = ms.set(CLIMATE_MODE_HEAT, 19.6f);
    ASSERT_TRUE(q_heat.has_value());
    EXPECT_FLOAT_EQ(*q_heat, 19.5f);
    EXPECT_FLOAT_EQ(*ms.get(CLIMATE_MODE_HEAT), 19.5f);

    auto q_cool = ms.set(CLIMATE_MODE_COOL, 24.3f);
    ASSERT_TRUE(q_cool.has_value());
    EXPECT_FLOAT_EQ(*q_cool, 24.5f);
    EXPECT_FLOAT_EQ(*ms.get(CLIMATE_MODE_COOL), 24.5f);

    EXPECT_FALSE(ms.set(CLIMATE_MODE_DRY, 22.0f).has_value());
    EXPECT_FALSE(ms.set(CLIMATE_MODE_OFF, 22.0f).has_value());
    EXPECT_FALSE(ms.set(CLIMATE_MODE_HEAT, 9.0f).has_value());
    EXPECT_FALSE(ms.set(CLIMATE_MODE_COOL, 32.0f).has_value());
    EXPECT_FALSE(ms.set(CLIMATE_MODE_AUTO, NAN).has_value());
}
