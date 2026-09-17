#pragma once

// Keep the portable Qt 6.4 harness compatible with the pinned Qt 6.11 API.
#ifndef QVERIFY_THROWS_EXCEPTION
#define QVERIFY_THROWS_EXCEPTION(exceptionType, ...) QVERIFY_EXCEPTION_THROWN((__VA_ARGS__), exceptionType)
#endif
