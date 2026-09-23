#include "src/app/debug_login_options.h"
#include "tests/support/test_check.h"

#include <QtCore/QStringList>

namespace {

using MeetingApp::DebugLoginOptionStatus;
using MeetingApp::ParseDebugLoginOptions;

void TestDisabledWithoutCredentials() {
	const auto normal = ParseDebugLoginOptions({
		QStringLiteral("Cohavora.exe")
	});
	TEST_CHECK(normal.status == DebugLoginOptionStatus::Disabled);
	TEST_CHECK(!normal.debugEnabled);

	const auto debug = ParseDebugLoginOptions({
		QStringLiteral("Cohavora.exe"), QStringLiteral("--debug")
	});
	TEST_CHECK(debug.status == DebugLoginOptionStatus::Disabled);
	TEST_CHECK(debug.debugEnabled);
}

void TestShortCredentialsRequireDebug() {
	constexpr auto kSecret = "temporary-secret";
	const auto rejected = ParseDebugLoginOptions({
		QStringLiteral("Cohavora.exe"),
		QStringLiteral("-u"), QStringLiteral("10000"),
		QStringLiteral("-p"), QString::fromLatin1(kSecret)
	});
	TEST_CHECK(rejected.status == DebugLoginOptionStatus::Invalid);
	TEST_CHECK(rejected.password.isEmpty());
	TEST_CHECK(!rejected.error.contains(QString::fromLatin1(kSecret)));

	auto accepted = ParseDebugLoginOptions({
		QStringLiteral("Cohavora.exe"), QStringLiteral("--debug"),
		QStringLiteral("-u"), QStringLiteral("10000"),
		QStringLiteral("-p"), QString::fromLatin1(kSecret)
	});
	TEST_CHECK(accepted.status == DebugLoginOptionStatus::Enabled);
	TEST_CHECK(accepted.debugEnabled);
	TEST_CHECK(accepted.account == QStringLiteral("10000"));
	TEST_CHECK(accepted.password == QString::fromLatin1(kSecret));
	accepted.clearPassword();
	TEST_CHECK(accepted.password.isEmpty());
}

void TestLongCredentialsAndUnrelatedArguments() {
	const auto accepted = ParseDebugLoginOptions({
		QStringLiteral("Cohavora.exe"), QStringLiteral("--locale"),
		QStringLiteral("zh_CN"), QStringLiteral("--username"),
		QStringLiteral("10001"), QStringLiteral("--debug"),
		QStringLiteral("--password"), QStringLiteral("123456")
	});
	TEST_CHECK(accepted.status == DebugLoginOptionStatus::Enabled);
	TEST_CHECK(accepted.account == QStringLiteral("10001"));
	TEST_CHECK(accepted.password == QStringLiteral("123456"));
}

void TestMalformedCredentialsAreRejected() {
	const auto missingPassword = ParseDebugLoginOptions({
		QStringLiteral("Cohavora.exe"), QStringLiteral("--debug"),
		QStringLiteral("-u"), QStringLiteral("10000")
	});
	TEST_CHECK(missingPassword.status == DebugLoginOptionStatus::Invalid);

	const auto missingValue = ParseDebugLoginOptions({
		QStringLiteral("Cohavora.exe"), QStringLiteral("--debug"),
		QStringLiteral("-p")
	});
	TEST_CHECK(missingValue.status == DebugLoginOptionStatus::Invalid);

	const auto duplicate = ParseDebugLoginOptions({
		QStringLiteral("Cohavora.exe"), QStringLiteral("--debug"),
		QStringLiteral("-u"), QStringLiteral("10000"),
		QStringLiteral("--username"), QStringLiteral("10001"),
		QStringLiteral("-p"), QStringLiteral("123456")
	});
	TEST_CHECK(duplicate.status == DebugLoginOptionStatus::Invalid);
	TEST_CHECK(duplicate.password.isEmpty());

	const auto debuggerIsNotDebug = ParseDebugLoginOptions({
		QStringLiteral("Cohavora.exe"), QStringLiteral("--debugger"),
		QStringLiteral("-u"), QStringLiteral("10000"),
		QStringLiteral("-p"), QStringLiteral("123456")
	});
	TEST_CHECK(debuggerIsNotDebug.status == DebugLoginOptionStatus::Invalid);
}

} // namespace

int main() {
	TestDisabledWithoutCredentials();
	TestShortCredentialsRequireDebug();
	TestLongCredentialsAndUnrelatedArguments();
	TestMalformedCredentialsAreRejected();
	return 0;
}
