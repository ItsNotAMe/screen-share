#include "BoundedRtcEventLog.h"
#include <iostream>
#include <string>

int main() {
    auto directory = std::filesystem::temp_directory_path() / ("screenshare-rtc-log-" + std::to_string(GetCurrentProcessId()));
    try {
        if (!std::filesystem::create_directory(directory)) throw std::runtime_error("Test directory exists");
        auto evidence = std::make_shared<proof::EventLogEvidence>();
        const auto path = directory / "test.rtc";
        {
            proof::BoundedRtcEventLog output(path, evidence);
            const std::string block(1024 * 1024, 'x');
            for (int i = 0; i < 8; ++i) if (!output.Write(block)) throw std::runtime_error("Unexpected write failure");
            if (output.Write("x") || output.IsActive() || !evidence->failed)
                throw std::runtime_error("Overflow was not rejected");
        }
        if (evidence->opened != 1 || evidence->closed != 1 || std::filesystem::file_size(path) != 8 * 1024 * 1024)
            throw std::runtime_error("Size or ownership mismatch");
        bool rejected = false;
        try { proof::BoundedRtcEventLog duplicate(path, std::make_shared<proof::EventLogEvidence>()); }
        catch (const std::runtime_error&) { rejected = true; }
        if (!rejected || std::filesystem::file_size(path) != 8 * 1024 * 1024)
            throw std::runtime_error("Existing evidence overwritten");
        std::filesystem::remove(path);
        std::filesystem::remove(directory);
        std::cout << "Bounded RTC output checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
