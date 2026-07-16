#include <unity.h>

#include <string>

#include "core/CommandQueue.h"
#include "core/CommandDispatcher.h"
#include "core/Config.h"
#include "core/EventBuffer.h"
#include "core/Models.h"
#include "core/ProtocolHelpers.h"
#include "core/PowerPolicy.h"
#include "core/StateReducer.h"
#include "core/TimeUtils.h"

using namespace chc;

void setUp() {}
void tearDown() {}

void test_config_requires_every_device_secret() {
  AppConfig config;
  TEST_ASSERT_FALSE(config.structurallyValid());
  config.wifi.ssid = "lan";
  config.tapo = {"user@example.com", "password"};
  config.dyson = {"SERIAL", "438K", "credential"};
  TEST_ASSERT_TRUE(config.structurallyValid());
  config.schemaVersion++;
  TEST_ASSERT_FALSE(config.structurallyValid());
}

void test_ac_reducer_limits_temperature_and_tracks_send_time() {
  AcState state;
  reduceAc(state, {DeviceId::Ac, CommandKind::SetTemperature, 30, 0, 0, 7}, 1234);
  TEST_ASSERT_EQUAL_UINT8(30, state.temperature);
  TEST_ASSERT_EQUAL_UINT32(1234, state.lastSentMs);
  reduceAc(state, {DeviceId::Ac, CommandKind::SetTemperature, 31, 0, 0, 8}, 1235);
  TEST_ASSERT_EQUAL_UINT8(30, state.temperature);
  reduceAc(state, {DeviceId::Ac, CommandKind::SetAcOnTimer, 120}, 1236);
  reduceAc(state, {DeviceId::Ac, CommandKind::SetAcOffTimer, 60}, 1237);
  TEST_ASSERT_EQUAL_UINT16(120, state.onTimerMinutes);
  TEST_ASSERT_EQUAL_UINT16(60, state.offTimerMinutes);
  reduceAc(state, {DeviceId::Ac, CommandKind::SetAcOnTimer, 1441}, 1238);
  TEST_ASSERT_EQUAL_UINT16(120, state.onTimerMinutes);
}

void test_dyson_topics_and_four_digit_fields() {
  TEST_ASSERT_EQUAL_STRING("438K/ABC-123/command",
                           dysonCommandTopic("438K", "ABC-123").c_str());
  TEST_ASSERT_EQUAL_STRING("438K/ABC-123/status/current",
                           dysonStatusTopic("438K", "ABC-123").c_str());
  TEST_ASSERT_EQUAL_STRING("0001", dysonFourDigits(1).c_str());
  TEST_ASSERT_EQUAL_STRING("0010", dysonFourDigits(10).c_str());
  TEST_ASSERT_EQUAL_STRING("0315", dysonFourDigits(315).c_str());
  TEST_ASSERT_EQUAL_STRING("0120", dysonFourDigits(120).c_str());
}

void test_reconnect_backoff_is_bounded() {
  TEST_ASSERT_EQUAL_UINT32(2000, nextReconnectDelay(1000));
  TEST_ASSERT_EQUAL_UINT32(60000, nextReconnectDelay(32000));
  TEST_ASSERT_EQUAL_UINT32(60000, nextReconnectDelay(60000));
}

void test_klap_sequence_and_padding_vectors() {
  const auto sequence = klapSequenceBytes(0x01020304);
  TEST_ASSERT_EQUAL_HEX8(0x01, sequence[0]);
  TEST_ASSERT_EQUAL_HEX8(0x04, sequence[3]);
  const auto padded = pkcs7Pad({'a', 'b', 'c'});
  TEST_ASSERT_EQUAL_UINT32(16, padded.size());
  TEST_ASSERT_EQUAL_HEX8(13, padded.back());
  const auto fullBlock = pkcs7Pad(std::vector<uint8_t>(16, 0x42));
  TEST_ASSERT_EQUAL_UINT32(32, fullBlock.size());
  TEST_ASSERT_EQUAL_HEX8(16, fullBlock.back());
}

void test_command_queue_preserves_order_and_rejects_overflow() {
  CommandQueue<DeviceCommand, 2> queue;
  TEST_ASSERT_TRUE(queue.push({DeviceId::Light, CommandKind::SetBrightness, 10}));
  TEST_ASSERT_TRUE(queue.push({DeviceId::Dyson, CommandKind::SetFanSpeed, 5}));
  TEST_ASSERT_FALSE(queue.push({DeviceId::Ac, CommandKind::TogglePower}));
  DeviceCommand command;
  TEST_ASSERT_TRUE(queue.pop(command));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DeviceId::Light), static_cast<int>(command.device));
  TEST_ASSERT_EQUAL_INT(10, command.value1);
  TEST_ASSERT_TRUE(queue.pop(command));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DeviceId::Dyson), static_cast<int>(command.device));
  TEST_ASSERT_FALSE(queue.pop(command));
}

void test_dispatcher_coalesces_rapid_absolute_controls() {
  CommandDispatcher<4> dispatcher;
  for (uint32_t id = 1; id <= 10; ++id)
    TEST_ASSERT_TRUE(dispatcher.push({DeviceId::Light, CommandKind::SetBrightness,
                                     static_cast<int32_t>(id * 10), 0, 0, id, id * 5}));
  TEST_ASSERT_EQUAL_UINT32(1, dispatcher.size());
  DeviceCommand command;
  TEST_ASSERT_TRUE(dispatcher.take(command));
  TEST_ASSERT_EQUAL_INT(100, command.value1);
  TEST_ASSERT_EQUAL_UINT32(10, command.id);
  TEST_ASSERT_EQUAL_UINT32(50, command.queuedAtMs);
  TEST_ASSERT_TRUE(dispatcher.push({DeviceId::Light, CommandKind::SetBrightness, 90, 0, 0, 11}));
  TEST_ASSERT_FALSE(dispatcher.take(command));
  TEST_ASSERT_TRUE(dispatcher.complete(10));
  TEST_ASSERT_TRUE(dispatcher.take(command));
  TEST_ASSERT_EQUAL_INT(90, command.value1);
}

void test_dedicated_ir_dispatcher_is_not_blocked_by_network_inflight() {
  CommandDispatcher<4> irDispatcher;
  CommandDispatcher<4> networkDispatcher;
  TEST_ASSERT_TRUE(networkDispatcher.push({DeviceId::Light, CommandKind::SetBrightness, 50, 0, 0, 1, 100}));
  DeviceCommand networkCommand;
  TEST_ASSERT_TRUE(networkDispatcher.take(networkCommand));
  TEST_ASSERT_TRUE(irDispatcher.push({DeviceId::Ac, CommandKind::SetTemperature, 23, 0, 0, 2, 120}));
  DeviceCommand irCommand;
  TEST_ASSERT_TRUE(irDispatcher.take(irCommand));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(DeviceId::Ac), static_cast<int>(irCommand.device));
  TEST_ASSERT_EQUAL_INT(23, irCommand.value1);
  TEST_ASSERT_EQUAL_UINT32(120, irCommand.queuedAtMs);
  TEST_ASSERT_EQUAL_UINT32(1, networkDispatcher.size());
}

void test_wrap_safe_deadlines() {
  const uint32_t started = 0xFFFFFFF0U;
  const uint32_t deadline = started + 32U;
  TEST_ASSERT_FALSE(deadlineReached(0xFFFFFFF8U, deadline));
  TEST_ASSERT_TRUE(deadlineReached(0x00000010U, deadline));
  TEST_ASSERT_EQUAL_UINT32(32, elapsedSince(0x00000010U, started));
}

void test_protocol_integrity_and_midea_vectors() {
  auto padded = pkcs7Pad({'t', 'e', 's', 't'});
  TEST_ASSERT_TRUE(validPkcs7Padding(padded.data(), padded.size()));
  padded[padded.size() - 2] ^= 1;
  TEST_ASSERT_FALSE(validPkcs7Padding(padded.data(), padded.size()));
  const uint8_t a[] = {1, 2, 3};
  const uint8_t b[] = {1, 2, 3};
  const uint8_t c[] = {1, 2, 4};
  TEST_ASSERT_TRUE(constantTimeEqual(a, b, sizeof(a)));
  TEST_ASSERT_FALSE(constantTimeEqual(a, c, sizeof(a)));
  TEST_ASSERT_TRUE(0xA18840FFFF56ULL == mideaCoolLowCelsiusVector(17));
  TEST_ASSERT_TRUE(0xA18846FFFF50ULL == mideaCoolLowCelsiusVector(23));
  TEST_ASSERT_TRUE(0xA1884DFFFF5DULL == mideaCoolLowCelsiusVector(30));
  TEST_ASSERT_TRUE(mideaChecksumValid(mideaCoolLowCelsiusVector(23)));
  TEST_ASSERT_TRUE(isMideaOneShotToggle(CommandKind::ToggleSwing));
  TEST_ASSERT_TRUE(isMideaOneShotToggle(CommandKind::ToggleTurbo));
  TEST_ASSERT_TRUE(isMideaOneShotToggle(CommandKind::ToggleEco));
  TEST_ASSERT_TRUE(isMideaOneShotToggle(CommandKind::ToggleClean));
  TEST_ASSERT_TRUE(isMideaOneShotToggle(CommandKind::ToggleLed));
}

void test_event_metadata_and_bounded_buffer() {
  EventBuffer<DeviceEvent, 2> events;
  events.push(makeEvent(DeviceId::Dyson, 1, EventResult::Succeeded, "one", ConfirmationKind::MqttState, 1, 1));
  events.push(makeEvent(DeviceId::Light, 2, EventResult::PartiallySucceeded, "partial", ConfirmationKind::KlapReadback, 5, 6));
  events.push(makeEvent(DeviceId::Ac, 3, EventResult::Succeeded, "three", ConfirmationKind::AssumedIr, 1, 1));
  DeviceEvent event;
  TEST_ASSERT_TRUE(events.pop(event));
  TEST_ASSERT_EQUAL_UINT32(2, event.commandId);
  TEST_ASSERT_EQUAL_UINT8(5, event.succeededTargets);
  TEST_ASSERT_EQUAL_UINT8(6, event.totalTargets);
  TEST_ASSERT_TRUE(events.pop(event));
  TEST_ASSERT_EQUAL_UINT32(3, event.commandId);
}

void test_config_rejects_oversized_fields() {
  AppConfig config;
  config.wifi = {"lan", "password"};
  config.tapo = {"user@example.com", "password"};
  config.dyson = {"SERIAL", "438K", "credential"};
  TEST_ASSERT_TRUE(config.structurallyValid());
  config.wifi.ssid = std::string(33, 'x');
  TEST_ASSERT_FALSE(config.structurallyValid());
}

void test_mixed_light_group_power_toggle() {
  LightState light;
  light.onlineDevices = 3;
  light.poweredOnDevices = 0;
  TEST_ASSERT_TRUE(lightGroupPowerTarget(light));
  light.mixed = true;
  light.mixedFields = 0x01;
  light.poweredOnDevices = 1;
  TEST_ASSERT_FALSE(lightGroupPowerTarget(light));
  light.poweredOnDevices = 3;
  TEST_ASSERT_FALSE(lightGroupPowerTarget(light));
  TEST_ASSERT_TRUE(nextLightGroupPowerTarget(light, true, false));
  TEST_ASSERT_FALSE(nextLightGroupPowerTarget(light, true, true));
}

void test_power_policy_motion_and_wrap_safe_timeouts() {
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenPowerMode::Awake),
                        static_cast<int>(screenPowerMode(29999, 0)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenPowerMode::Dimmed),
                        static_cast<int>(screenPowerMode(30000, 0)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenPowerMode::Sleeping),
                        static_cast<int>(screenPowerMode(120000, 0)));
  TEST_ASSERT_EQUAL_INT(static_cast<int>(ScreenPowerMode::Dimmed),
                        static_cast<int>(screenPowerMode(0x00007520U, 0xFFFFFFF0U)));
  TEST_ASSERT_FALSE(motionDetected(0.0F, 0.0F, 1.0F, 0.01F, 0.0F, 1.0F,
                                   1.0F, 1.0F, 1.0F));
  TEST_ASSERT_TRUE(motionDetected(0.20F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F,
                                  0.0F, 0.0F, 0.0F));
  TEST_ASSERT_TRUE(motionDetected(0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F,
                                  8.0F, 8.0F, 0.0F));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_config_requires_every_device_secret);
  RUN_TEST(test_ac_reducer_limits_temperature_and_tracks_send_time);
  RUN_TEST(test_dyson_topics_and_four_digit_fields);
  RUN_TEST(test_reconnect_backoff_is_bounded);
  RUN_TEST(test_klap_sequence_and_padding_vectors);
  RUN_TEST(test_command_queue_preserves_order_and_rejects_overflow);
  RUN_TEST(test_dispatcher_coalesces_rapid_absolute_controls);
  RUN_TEST(test_dedicated_ir_dispatcher_is_not_blocked_by_network_inflight);
  RUN_TEST(test_wrap_safe_deadlines);
  RUN_TEST(test_protocol_integrity_and_midea_vectors);
  RUN_TEST(test_event_metadata_and_bounded_buffer);
  RUN_TEST(test_config_rejects_oversized_fields);
  RUN_TEST(test_mixed_light_group_power_toggle);
  RUN_TEST(test_power_policy_motion_and_wrap_safe_timeouts);
  return UNITY_END();
}
