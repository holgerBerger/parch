/*
 * archiver – CLI entry point (stub).
 *
 * Full create/extract/list/append commands land in PLAN.md Phase 5.
 */
#include <spdlog/spdlog.h>

int main(int argc, char** argv) {
    if (argc < 2) {
        spdlog::info("usage: arch <create|extract|list|append> [args...] "
                     "(CLI not yet implemented — see PLAN.md Phase 5)");
        return 1;
    }
    spdlog::error("arch {}: not implemented yet", argv[1]);
    return 2;
}
