#include <unity.h>

#include <string>

#include "core/CommandQueue.h"
#include "core/Config.h"
#include "core/Models.h"
#include "core/ProtocolHelpers.h"
#include "core/StateReducer.h"

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

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_config_requires_every_device_secret);
  RUN_TEST(test_ac_reducer_limits_temperature_and_tracks_send_time);
  RUN_TEST(test_dyson_topics_and_four_digit_fields);
  RUN_TEST(test_reconnect_backoff_is_bounded);
  RUN_TEST(test_klap_sequence_and_padding_vectors);
  RUN_TEST(test_command_queue_preserves_order_and_rejects_overflow);
  return UNITY_END();
}
