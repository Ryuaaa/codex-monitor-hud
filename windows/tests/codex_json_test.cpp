#include "codex/codex_json_win32.h"

#include <winrt/base.h>

#include <iostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

using codex_monitor::codex::AccountData;
using codex_monitor::codex::CodexDataState;
using codex_monitor::codex::MethodFailureKind;
using codex_monitor::codex::ProcessLocalThread;
using codex_monitor::codex::ProcessLocalThreadStatus;

int failures = 0;

void Expect(bool condition, const char* message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

template <typename T, typename = void>
struct HasEmailMember : std::false_type {};

template <typename T>
struct HasEmailMember<T, std::void_t<decltype(std::declval<T>().email)>>
    : std::true_type {};

template <typename T, typename = void>
struct HasPreviewMember : std::false_type {};

template <typename T>
struct HasPreviewMember<T, std::void_t<decltype(std::declval<T>().preview)>>
    : std::true_type {};

template <typename T, typename = void>
struct HasIdMember : std::false_type {};

template <typename T>
struct HasIdMember<T, std::void_t<decltype(std::declval<T>().id)>>
    : std::true_type {};

template <typename T, typename = void>
struct HasCwdMember : std::false_type {};

template <typename T>
struct HasCwdMember<T, std::void_t<decltype(std::declval<T>().cwd)>>
    : std::true_type {};

template <typename T, typename = void>
struct HasPathMember : std::false_type {};

template <typename T>
struct HasPathMember<T, std::void_t<decltype(std::declval<T>().path)>>
    : std::true_type {};

static_assert(!HasEmailMember<AccountData>::value,
              "account model must not retain email");
static_assert(!HasPreviewMember<ProcessLocalThread>::value,
              "thread model must not retain preview text");
static_assert(!HasIdMember<ProcessLocalThread>::value,
              "thread model must not retain thread identifiers");
static_assert(!HasCwdMember<ProcessLocalThread>::value,
              "thread model must not retain working directories");
static_assert(!HasPathMember<ProcessLocalThread>::value,
              "thread model must not retain rollout paths");

void TestInitializeCodexHomeValidation() {
    const auto drivePath =
        codex_monitor::codex::ParseInitializeCodexHomeResultJson(
            R"json({"codexHome":"C:\\Users\\Codex User\\.codex"})json");
    Expect(drivePath && *drivePath ==
                            std::filesystem::path(L"C:\\Users\\Codex User\\.codex"),
           "an absolute Windows drive path must be accepted");

    const auto uncPath =
        codex_monitor::codex::ParseInitializeCodexHomeResultJson(
            R"json({"codexHome":"\\\\server\\share\\.codex"})json");
    Expect(uncPath && *uncPath ==
                          std::filesystem::path(L"\\\\server\\share\\.codex"),
           "an absolute Windows UNC path must be accepted");

    for (const std::string_view json : {
             R"json({"server":"compatible-without-codex-home"})json",
             R"json({"codexHome":null})json",
             R"json({"codexHome":"..\\private"})json",
             R"json({"codexHome":"C:drive-relative"})json",
             R"json({"codexHome":"\\root-relative"})json",
             R"json({"codexHome":"C:\\Safe\u0000evil"})json",
             R"json({"codexHome":42})json",
             R"json(not-json)json",
         }) {
        Expect(!codex_monitor::codex::ParseInitializeCodexHomeResultJson(json),
               "missing, invalid, relative, or NUL-bearing Codex homes must be rejected");
    }
}

void TestCompletePayloadsAndCodexBucketPriority() {
    const auto rateLimits = codex_monitor::codex::ParseRateLimitsResultJson(R"json(
        {
          "rateLimits": {
            "planType": "free",
            "primary": {"usedPercent": 99, "windowDurationMins": 60, "resetsAt": 1}
          },
          "rateLimitsByLimitId": {
            "other": {"primary": {"usedPercent": 80}},
            "codex": {
              "planType": "pro",
              "primary": {
                "usedPercent": 25,
                "windowDurationMins": 300,
                "resetsAt": 1786320000
              },
              "secondary": {
                "usedPercent": 40,
                "windowDurationMins": 10080,
                "resetsAt": 1786924800
              },
              "ignoredFutureField": {"anything": true}
            }
          }
        }
    )json");
    Expect(rateLimits.ok(), "complete rate-limit payload must parse");
    Expect(rateLimits.value && rateLimits.value->selectedCodexLimitId,
           "rateLimitsByLimitId.codex must win over legacy rateLimits");
    Expect(rateLimits.value && rateLimits.value->planType == L"pro",
           "selected codex bucket plan type must be retained");
    Expect(rateLimits.value && rateLimits.value->primary &&
               rateLimits.value->primary->usedPercent == 25 &&
               rateLimits.value->primary->windowDurationMinutes == 300,
           "primary quota window must retain schema fields");
    Expect(rateLimits.value && rateLimits.value->secondary &&
               rateLimits.value->secondary->usedPercent == 40,
           "secondary quota window must parse independently");

    const auto account = codex_monitor::codex::ParseAccountResultJson(R"json(
        {
          "requiresOpenaiAuth": false,
          "account": {
            "type": "chatgpt",
            "email": "must-not-survive@example.com",
            "planType": "business",
            "unknownInteger": 9007199254740993
          }
        }
    )json");
    Expect(account.ok() && account.value && account.value->planType == L"business",
           "account/read must retain only planType and ignore unknown fields");

    const auto usage = codex_monitor::codex::ParseUsageResultJson(R"json(
        {
          "dailyUsageBuckets": [
            {"startDate": "2026-08-09", "tokens": 1234},
            {"startDate": "2026-08-10", "tokens": 5678, "ignored": "字段"}
          ],
          "summary": {
            "currentStreakDays": 3,
            "lifetimeTokens": 90000,
            "longestRunningTurnSec": 7200,
            "longestStreakDays": 9,
            "peakDailyTokens": 25000
          }
        }
    )json");
    Expect(usage.ok() && usage.value && usage.value->dailyUsageBuckets &&
               usage.value->dailyUsageBuckets->size() == 2,
           "daily usage buckets must parse");
    Expect(usage.value && usage.value->summary.longestRunningTurnSeconds == 7200 &&
               usage.value->summary.peakDailyTokens == 25000,
           "usage summary fields must parse");

    const auto threads = codex_monitor::codex::ParseThreadListResultJson(R"json(
        {
          "data": [
            {
              "id": "secret-thread-id",
              "name": "中文任务标题",
              "recencyAt": 1786320123,
              "status": {"type": "active", "activeFlags": []},
              "preview": "SECRET PREVIEW MUST NOT LEAK",
              "cwd": "C:\\SecretProject",
              "path": "C:\\SecretProject\\rollout.jsonl"
            },
            {
              "name": "第二个任务",
              "recencyAt": 1786320000,
              "status": {"type": "futureStatus"}
            }
          ],
          "nextCursor": "ignored-cursor"
        }
    )json");
    Expect(threads.ok() && threads.value && threads.value->threads.size() == 2,
           "thread/list data must parse without private metadata");
    Expect(threads.value && threads.value->threads[0].name == L"中文任务标题",
           "Unicode thread names must survive parsing");
    Expect(threads.value && threads.value->threads[0].processLocalStatus ==
                                ProcessLocalThreadStatus::kActive,
           "active status must be explicitly process-local");
    Expect(threads.value && threads.value->threads[1].processLocalStatus ==
                                ProcessLocalThreadStatus::kUnknown,
           "future string status values must degrade to unknown");
}

void TestMissingNullAndLegacyShapes() {
    const auto rateLimits = codex_monitor::codex::ParseRateLimitsResultJson(R"json(
        {
          "rateLimitsByLimitId": null,
          "rateLimits": {"planType": null, "primary": null, "secondary": null}
        }
    )json");
    Expect(rateLimits.ok() && rateLimits.value && !rateLimits.value->primary &&
               !rateLimits.value->secondary,
           "nullable rate-limit fields must remain unavailable, not become zero");

    const auto account = codex_monitor::codex::ParseAccountResultJson(
        R"json({"requiresOpenaiAuth": true, "account": null})json");
    Expect(account.ok() && account.value && !account.value->planType,
           "null account must parse without inventing a plan");

    const auto usage = codex_monitor::codex::ParseUsageResultJson(R"json(
        {
          "dailyUsageBuckets": null,
          "summary": {
            "currentStreakDays": null,
            "lifetimeTokens": null,
            "longestRunningTurnSec": null,
            "longestStreakDays": null,
            "peakDailyTokens": null
          }
        }
    )json");
    Expect(usage.ok() && usage.value && !usage.value->dailyUsageBuckets &&
               !usage.value->summary.lifetimeTokens,
           "null usage values must remain unavailable");

    const auto threads = codex_monitor::codex::ParseThreadListResultJson(R"json(
        {
          "data": null,
          "threads": [
            {"name": null, "recencyAt": null, "status": null}
          ]
        }
    )json");
    Expect(threads.ok() && threads.value && threads.value->usedLegacyThreadsField &&
               threads.value->threads.size() == 1,
           "legacy threads must be used when data is absent or null");
    Expect(threads.value && !threads.value->threads[0].name &&
               !threads.value->threads[0].recencyAtUnixSeconds &&
               !threads.value->threads[0].processLocalStatus,
           "nullable thread fields must not be fabricated");
}

void TestStrictTypesUnsafeIntegersAndMalformedJson() {
    const auto wrongType = codex_monitor::codex::ParseAccountResultJson(
        R"json({"account":{"planType":42}})json");
    Expect(!wrongType.ok() && wrongType.failure &&
               wrongType.failure->kind == MethodFailureKind::kUnexpectedType,
           "known fields with the wrong type must fail the method mapping");

    const auto unsafeInteger = codex_monitor::codex::ParseUsageResultJson(R"json(
        {
          "dailyUsageBuckets": [
            {"startDate": "2026-08-10", "tokens": 9007199254740993}
          ],
          "summary": {}
        }
    )json");
    Expect(!unsafeInteger.ok() && unsafeInteger.failure &&
               unsafeInteger.failure->kind == MethodFailureKind::kUnsafeInteger,
           "integers outside the exact JSON number range must be rejected");

    const auto malformed = codex_monitor::codex::ParseThreadListResultJson(
        R"json({"data":[})json");
    Expect(!malformed.ok() && malformed.failure &&
               malformed.failure->kind == MethodFailureKind::kMalformedJson,
           "malformed JSON must be a method-level failure");

    const auto fractional = codex_monitor::codex::ParseUsageResultJson(R"json(
        {"dailyUsageBuckets":[],"summary":{"lifetimeTokens":1.5}}
    )json");
    Expect(!fractional.ok() && fractional.failure &&
               fractional.failure->kind == MethodFailureKind::kUnexpectedType,
           "fractional JSON numbers must not enter integer fields");
}

void TestMethodFailuresRetainOnlyTheirOwnLastValue() {
    CodexDataState state;
    codex_monitor::codex::ApplyMethodResult(
        state.account,
        codex_monitor::codex::ParseAccountResultJson(
            R"json({"account":{"planType":"pro"}})json"));
    codex_monitor::codex::ApplyMethodResult(
        state.rateLimits,
        codex_monitor::codex::ParseRateLimitsResultJson(
            R"json({"rateLimits":{"primary":{"usedPercent":10}}})json"));
    codex_monitor::codex::ApplyMethodResult(
        state.rateLimits,
        codex_monitor::codex::ParseRateLimitsResultJson("not-json"));

    Expect(state.rateLimits.lastValue && state.rateLimits.lastFailure &&
               state.rateLimits.lastValue->primary &&
               state.rateLimits.lastValue->primary->usedPercent == 10,
           "a method failure must retain that method's last successful value");
    Expect(state.account.lastValue && !state.account.lastFailure &&
               state.account.lastValue->planType == L"pro",
           "one method failure must not clear or fail another method state");
}

}  // namespace

int main() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    TestInitializeCodexHomeValidation();
    TestCompletePayloadsAndCodexBucketPriority();
    TestMissingNullAndLegacyShapes();
    TestStrictTypesUnsafeIntegersAndMalformedJson();
    TestMethodFailuresRetainOnlyTheirOwnLastValue();
    if (failures != 0) return 1;
    std::cout << "codex_json_tests=pass\n";
    return 0;
}
