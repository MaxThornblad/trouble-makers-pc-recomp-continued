#include "gem_hints.h"

#include <algorithm>
#include <atomic>

#include "cheats.h"
#include "recomp.h"

#ifdef MM_HAS_GRAPHICS
#include "imgui.h"
#endif

namespace mm::gem_hints {
namespace {

struct Hint {
    int row;              // progression row (0x80178162)
    const char* label;
    const char* name;
    const char* text;     // nullptr: the wiki page could not be read
};

// From each level's "Golden Gem" section on the Mischief Makers Wiki
// (https://mischiefmakers.fandom.com, CC BY-SA 3.0). 1-1, 1-7, 2-1, 2-2,
// 2-5..2-10, 3-3, 3-4, 3-9, 4-5, 4-8, 4-9 and 5-3 are the wiki text as supplied
// by the user; the rest are condensed.
constexpr Hint kHints[] = {
    { 2, "1-1", "Meet Marina!!",
      "Under the first house there is a bronze clancer statue. Standing nearby is a "
      "clancer. Marina must pick this person up and throw him at the statue, breaking "
      "it and revealing a golden gem." },
    { 3, "1-2", "Meet Calina!!",
      "On the first screen, go to the green blocks just before the fire. A platform of "
      "purple blocks sits above their far end: boost up and to the right onto it to "
      "find the gem." },
    { 4, "1-3", "Clanball Land",
      "In the roller-coaster section with four different clanballs, grab the fast "
      "orange one and shake it while passing over a red block. The bomb it drops blasts "
      "open nearby blocks and reveals a hidden star; take the star to reach an area you "
      "couldn't get to before and grab the gem quickly." },
    { 5, "1-4", "Spike Land",
      "After the first three laser-firing spike balls, look for a hill. Boost upward, "
      "then move to the right of the hill into a nook of green blocks. Dodge the spike "
      "balls there and take the gem." },
    { 6, "1-5", "3 Clancer Kids",
      "Go back to the first spring, shake it so it launches up-right at an angle, pull "
      "it back fully and boost up-right just before falling to land on a blue block. "
      "Climb the green blocks right to a sparkling cylinder block, pull out a Clanbomb "
      "and quickly boost off that roof toward where you chased the third kid. Bomb the "
      "red blocks at the top and shake the father Clancer hiding behind them to make him "
      "drop the gem (bringing him home gives nothing)." },
    { 7, "1-6", "Blockman Rises",
      "The last of the three bomb-throwing Clancers, the wealthy one, has it. Get off "
      "Blockman, dodge the bombs and shake him." },
    { 8, "1-7", "Wormin' Up!!",
      "Shake the Worm." },
    { 9, "1-8", "Crisis: Nepton",
      "Beat every wave of enemies without letting Teran or your other Clancer ally take "
      "so much damage that they break down crying. Four gems trapped under the "
      "battlefield are then released; the golden one is on the far right." },
    { 10, "1-9", "Western World",
      "After beating the cowboy leader, go to the top-right area where spikes circle "
      "Clancer blocks. Two bombable walls are in the way, so carry a bomb there twice, "
      "one per wall. Touching a spike makes you drop the bomb, but cleared spikes don't "
      "come back." },
    { 11, "1-10", "Volcano!!",
      "Keep going until you reach a big triangle made of red and blue gems, then follow "
      "the path up and to the left to the gem." },

    { 12, "2-1", "Sea of Lava",
      "When you find the golden gem, collect it by sliding from the platforms beside "
      "it. At the end of the level." },
    { 13, "2-2", "Vertigo!!",
      "When you get to the end of the level, destroy the blocks then as they are "
      "falling apart climb on top of them and get on the elevator to the golden gem." },
    { 14, "2-3", "Sink or Float!",
      "At the end of the level, follow the arrow made of gems: slide-jump, then "
      "air-dash the whole distance to a sinking block with another gem arrow. Quickly "
      "repeat the same jump to the next platform, where a star warps you to the gem." },
    { 15, "2-4", "Hot Rush",
      "Speed is everything; expect several tries and lots of slide-jumping. Rush to the "
      "first red star and jump in (if the miner below has just started running right, "
      "you're on pace). Run over, slide-jump the cylinder wall and dash to the white "
      "clanball, pulling down on it before the ground breaks. Go back through the "
      "tunnel and over the wall, keep rushing, teleport through the second star, then "
      "dash to that white clanball and pull it down for the gem." },
    { 16, "2-5", "Searin' Swing!",
      "When you get to the end of the level, keep going right on the clanswings and "
      "you will come to the golden gem sitting on an island surrounded by red and "
      "blue gems." },
    { 17, "2-6", "Flambee!!",
      "After grabbing his head, shake him till you fall off. Repeat until well "
      "shaken. When he drops his loot, jump off and rush over to your hard earned "
      "golden gem before it disappears." },
    { 18, "2-7", "Tightrope Ride",
      "Go through the level till you come to the clanball elevator. When you see the "
      "end star gate, keep going up until you see a platform on the left. Collect "
      "your golden gem and leave." },
    { 19, "2-8", "Freefall!!",
      "When Marina first begins to fall, go straight down the middle, then take a "
      "right on the fork. There will be a break in the chain of spikeballs, and if "
      "she passes through this, and then moves immediately to the left, the golden "
      "gem will be against the wall of blue blocks. If the gem is missed while "
      "falling she can jump back up the wall along the ledges, expending health along "
      "the spikeballs, or reach the very bottom and start the level again from the "
      "top." },
    { 20, "2-9", "Magma Rafts",
      "The golden gem in this stage is fairly straightforward. When Marina comes to a "
      "clanball with a spring, there will be a platform below its path where a "
      "clancer stands with a flower on its head. If she drops to this platform and "
      "shakes him, he will drop the gem." },
    { 21, "2-10", "Seasick Climb",
      "Follow the level until you reach the ending star gate. Keep going up after "
      "that, shaking clanballs as you go, until there is a break in the left wall "
      "that you can fit through. Drop down and collect your prize to add to your "
      "golden gem collection, then make your way back to the star gate." },
    { 22, "2-11", "Migen Brawl!!",
      "Defeat both Migens without taking any damage." },

    { 24, "3-1", "Clanpot Shake",
      "Make it from two green gems. Catch the six blue gems from the orange clanball at "
      "the start in the Clanpot and combine them into one green gem. Do the same with "
      "the six blue gems from a second orange clanball later on, then mix the two green "
      "gems together into the golden gem." },
    { 25, "3-2", "Clance War",
      "Near the start, grab and shake the Clancer wearing a Santa hat. Be quick and "
      "careful: the Super Clancers throw a bomb that kills him if you take too long." },
    { 26, "3-3", "Missile Surf!!",
      "Stay on the missile after the end star and keep going up until you reach the "
      "gold gem." },
    { 27, "3-4", "Clanball Lift!",
      "Near the end, at the big Clancer Swing (made of Clancers) the gold gem is in "
      "the centre just above the rope. You could either use the swing to reach it "
      "(which is what is most likely intended), or from the post just before, slide "
      "jump and boost to get it." },
    { 28, "3-5", "Go Marzen 64",
      "In the second area, above the open abyss, shake the white clanball so a Clanbomb "
      "drops further right; catch it before it lands and carry it to the Clanpot caged "
      "in red blocks. Use the bomb to chain-explode the blocks, catch the Clanpot before "
      "it falls and shake it to fuse its three green missile Clancers into one giant "
      "jetpack Clancer. Ride it up to the gem, floating high among a cluster of gems." },
    { 29, "3-6", "Chilly Dog!!",
      "Grab the Chilly Dog's head and shake it: each shake has a 1 in 64 chance of "
      "dropping the gem." },
    { 30, "3-7", "Snowstorm Maze",
      "On the first screen, don't put the jump Clancer in its pen: ride it to the top "
      "left. From the tricycle pen, bounce across the heads of several spike balls to "
      "the highest corner, where the gem is." },
    { 31, "3-8", "LUNAR!!",
      "In this first battle with Lunar, hit him with energy balls." },
    { 32, "3-9", "The Day Before",
      "To get the golden gem, you must make several successful jumps over the jump "
      "rope that some clancers are practising out. Every successful jump gives you a "
      "red gem." },
    { 33, "3-10", "The Day Of",
      "Finish the 100m Dash in under eleven seconds. Mash the D-pad (not the C buttons) "
      "and start the instant \"Go\" appears - anticipate it rather than react. You get "
      "three false starts to practise the timing." },
    { 34, "3-11", "Cat-astrophe!!",
      "Beat Cat Clancer at dodgeball without stepping over the line and without taking "
      "any damage." },
    { 35, "3-12", "CERBERUS Alpha",
      "Defeat Lunar on CERBERUS without taking a single hit - probably the hardest "
      "no-damage boss." },

    { 37, "4-1", "Rolling Rock!!",
      "Go to the warp star next to the red blocks holding the gold star (the gem is "
      "already visible). The warp drops you into a box with the gem; to get out, set "
      "off a bomb from the third pipe." },
    { 38, "4-2", "Toadly Raw!!",
      "The Toad drops it. After a while it laughs and summons a cloud with a little "
      "dancing Clancer: catch the lightning the cloud fires down, let it grow to full "
      "size and throw it at the Toad. Shake the Toad a lot and avoid hurting it other "
      "ways to make the summon more likely." },
    { 39, "4-3", "7 Clancer Kids",
      "After beating Sarie, the purple Clancer kid hiding in the rightmost chamber of "
      "the Clanpot area, shake him and then talk to him to get the gem." },
    { 40, "4-4", "Rescue! Act 1",
      "It sits on the platforms spelling MARINA, reached by a chain of warp stars that "
      "vanish once used, so any slip means restarting. From the big hill with the green "
      "pipe, take its top-left platform and keep left to a warp star inside the hill "
      "(the map's only healing). Then use the sky platform's warp star (two blinking "
      "lights), go right past another warp star and lights, time your jumps upward, jump "
      "right to a warp star, time it to the bottom and enter. Cross the MARINA platforms "
      "to the last A, where the gem sits by a warp star and three lights: take the middle "
      "light and watch the fast last one." },
    { 41, "4-5", "Rescue! Act 2",
      "Make Calina destroy the most right blue block (in the bottom row) and then "
      "triple jump back up after getting it." },
    { 42, "4-6", "TARUS!!",
      "Wait for Tarus's wind-up punch, grab his fist and throw him behind you. If he "
      "crashes into the cave wall he drops the gem - but you must not take any damage." },
    { 43, "4-7", "Ghost Catcher!",
      "At the end of the level, inside a trio of statues below the two fire-casting "
      "ghosts. Grab a bomb from the cylinder near the start, hurry to the statues "
      "before it explodes and blow them up." },
    { 44, "4-8", "Aster's Tryke!",
      "The golden gem can be found at the very end of the stage, right before Marina "
      "meets King Aster awaiting his tricycle. It is possible to hit it while taking "
      "the final jump with the tryke, but if the gem is overshot for some reason, it "
      "is simple enough to stop using Marina's jets, dismount from the tricycle, and "
      "walk backwards to where the gem floats in plain sight. She would have to grab "
      "it quickly before the wave of water destroys it and she must start over." },
    { 45, "4-9", "Moley Cow!!",
      "When Moley is knocked out, jump on his belly. Each jump has a 1-in-64 chance "
      "of making it spit a gold gem." },
    { 46, "4-10", "Aster's Maze",
      "In the green section, right beside warp number 4: shake the nearby hint-giving "
      "Clancer and he drops it." },
    { 47, "4-11", "SASQUATCH Beta",
      "Defeat Tarus and Sasquatch without taking any damage." },

    { 49, "5-1", "Clance War II",
      "In the fourth section, the first black Clancer you meet has it: shake him. Move "
      "carefully so no guns kill him before you reach him." },
    { 50, "5-2", "Counterattack",
      "Wait until the Marzen 64 has released five orange clanballs and charged its "
      "laser. Stand slightly left of halfway between the mech and the right edge of the "
      "screen and catch the laser; the gem appears and is collected automatically." },
    { 51, "5-3", "Bee's the one!",
      "The 3rd missile you shake will have the golden gem in it. Note: Do not shake "
      "the Missile too close on the left, as this stage auto scrolls and the gold gem "
      "will become unreachable." },
    { 52, "5-4", "MERCO!!",
      "Grab Merco's lance and shake it just before he pulls back to slash, and finish "
      "the fight without taking any damage." },
    { 53, "5-5", "Trapped!?",
      "In the first building (the puzzle), solve it as normal to reach the door but "
      "don't go through. Blow up the blocks the bombs spawn over (normally a statue goes "
      "there), enter the star inside and shake the clanball you land at to make a second "
      "one. Go through the star, shake the new clanball to spawn a third beside the "
      "first, return through the star the red blocks used to block and shake the newest "
      "clanball: the one it makes holds the gem." },
    { 54, "5-6", "PHOENIX Gamma",
      "Defeat Merco on PHOENIX without taking a single hit." },
    { 56, "5-7", "Inner Struggle",
      "During the fight with Emperor Leo, wait for a bubble with the word \"Dire\", "
      "shake it so it turns into \"Lucky\", and throw it at Leo while you're out of his "
      "line of fire - the gem falls out." },
    { 57, "5-8", "Final Battle",
      "Defeat THE Beastector without taking any damage." },
};

constexpr uint32_t kGameState = 0x800BE4F0u;
constexpr uint32_t kGamePaused = 0x800BE4E8u;
constexpr uint32_t kCurrentStage = 0x80178162u;
constexpr uint32_t kGoldGemsHi = 0x80171B10u;   // rows 32..63
constexpr uint32_t kGoldGemsLo = 0x80171B14u;   // rows 0..31

std::atomic<bool> g_paused{false};
std::atomic<int> g_row{-1};
std::atomic<bool> g_collected{false};

gpr addr(uint32_t vaddr) {
    return static_cast<gpr>(static_cast<int32_t>(vaddr));
}

const Hint* find_hint(int row) {
    for (const Hint& h : kHints) {
        if (h.row == row) return &h;
    }
    return nullptr;
}

} // namespace

void publish(uint8_t* rdram) {
    const bool paused = MEM_HU(0, addr(kGameState)) == 6 && MEM_HU(0, addr(kGamePaused)) != 0;
    const int row = MEM_HU(0, addr(kCurrentStage));
    bool collected = false;
    if (row >= 0 && row < 64) {
        const uint32_t word = static_cast<uint32_t>(MEM_W(0, addr(row < 32 ? kGoldGemsLo : kGoldGemsHi)));
        collected = ((word >> (row & 31)) & 1u) != 0;
    }
    g_row.store(row, std::memory_order_relaxed);
    g_collected.store(collected, std::memory_order_relaxed);
    g_paused.store(paused, std::memory_order_release);
}

#ifdef MM_HAS_GRAPHICS
void draw_overlay() {
    if (!mm::cheats::gem_hints_enabled() || !g_paused.load(std::memory_order_acquire)) {
        return;
    }
    const Hint* hint = find_hint(g_row.load(std::memory_order_relaxed));
    if (hint == nullptr) return;
    const bool collected = g_collected.load(std::memory_order_relaxed);

    const ImGuiIO& io = ImGui::GetIO();
    const float scale = std::max(1.0f, io.DisplaySize.y / 720.0f);
    const float width = std::min(io.DisplaySize.x - 32.0f, 560.0f * scale);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y - 24.0f * scale),
                            ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.82f);
    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##gem_hint", nullptr, flags)) {
        ImGui::SetWindowFontScale(scale * 1.15f);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.00f, 0.85f, 0.25f, 1.0f));
        ImGui::Text("Golden gem  -  %s %s", hint->label, hint->name);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (collected) {
            ImGui::TextColored(ImVec4(0.45f, 0.95f, 0.55f, 1.0f), "  (collected)");
        } else {
            ImGui::TextDisabled("  (not collected yet)");
        }
        ImGui::Separator();
        if (hint->text != nullptr) {
            ImGui::TextWrapped("%s", hint->text);
        } else {
            ImGui::TextDisabled("No hint yet: the wiki page for this level couldn't be read.");
        }
        ImGui::SetWindowFontScale(scale * 0.85f);
        ImGui::TextDisabled("Source: Mischief Makers Wiki (mischiefmakers.fandom.com), CC BY-SA 3.0");
    }
    ImGui::End();
}
#endif

} // namespace mm::gem_hints
