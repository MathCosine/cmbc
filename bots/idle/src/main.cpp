// A bot that drafts and then never acts: its champions stay on their spawn. Playing
// against it measures how fast a bot farms with nobody contesting it (tools/pve.py).
#include <cambc.h>
using namespace cambc;
int main() {
    while (true) {
        try {
            if (is_draft()) {
                const auto chosen = draft::current_draft(team::ally);
                draft::pick(static_cast<champion_draft>(chosen.size()));
            }
        } catch (const std::exception&) {
        }
        end_turn();
    }
}
