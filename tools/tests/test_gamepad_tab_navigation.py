#!/usr/bin/env python3
"""Unit tests for gamepad shoulder button tab navigation (Option 4 / G8).

Verifies tab cycling for:
- SpellBookFrame (SpellBookSkillLineTab1..N)
- CharacterFrame (CharacterFrameTab1..N, skipping hidden tabs such as pet)
- PlayerTalentFrame / TalentFrame (the 3 talent tree tabs)
- TradeSkillFrame / CraftFrame (tabs if present)
- Non-tabbed panels (returns false, falling back to scrolling)
"""
from pathlib import Path
import subprocess
import tempfile
import unittest
import os
import re

ROOT = Path(__file__).resolve().parents[2]
INPUT_CPP = ROOT / 'src/addons/local_framexml_input.cpp'

def make_run_cycle_function():
    content = INPUT_CPP.read_text()
    start_marker = 'const std::string lua ='
    end_marker = 'if(engine_->executeString(lua))'
    start_pos = content.index(start_marker) + len(start_marker)
    end_pos = content.index(end_marker, start_pos)
    block = content[start_pos:end_pos].strip().rstrip(';')
    cleaned = block.replace('panelName', 'panel')
    cleaned = cleaned.replace('std::to_string(delta)', 'delta')
    parts = []
    for line in cleaned.splitlines():
        line = line.strip()
        for s in re.findall(r'"((?:[^"\\]|\\.)*)"', line):
            parts.append(s.replace('\\"', '"').replace('\\n', '\n'))
    body = "".join(parts)
    lines = body.splitlines()
    body_lines = [l for l in lines if not l.startswith('local panel =') and not l.startswith('local delta =')]
    body_without_args = "\n".join(body_lines)
    return "function runCycle(panel, delta)\n" + body_without_args + "\nreturn __WoWPSTabSwitched\nend\n"

import shutil

def find_lua():
    if 'LUA' in os.environ and shutil.which(os.environ['LUA']):
        return os.environ['LUA']
    for candidate in ['luajit', 'lua5.1', 'lua', 'lua5.2', 'lua5.3', 'lua5.4']:
        found = shutil.which(candidate)
        if found:
            return found
    return 'luajit'

def run_lua_test(test_lua_code):
    lua_bin = find_lua()
    full_script = make_run_cycle_function() + "\n" + test_lua_code
    with tempfile.NamedTemporaryFile('w', suffix='.lua', delete=False) as f:
        f.write(full_script)
        path = f.name
    try:
        res = subprocess.run([lua_bin, path], capture_output=True, text=True)
        if res.returncode != 0:
            raise RuntimeError(f"Lua failed (exit {res.returncode}):\nSTDOUT: {res.stdout}\nSTDERR: {res.stderr}")
        return res.stdout
    finally:
        os.unlink(path)

class TestGamepadTabNavigation(unittest.TestCase):
    def run_cycle(self, setup_code, panel_name, delta):
        code = extract_cycle_lua(panel_name, delta)
        full_script = f"""
{setup_code}
{code}
print(__WoWPSTabSwitched and "SUCCESS" or "NOT_SWITCHED")
"""
        out = run_lua_test(full_script)
        return "SUCCESS" in out

    def test_spellbook_cycling(self):
        setup = """
SpellBookFrame = { selectedSkillLine = 1 }
MAX_SKILLLINE_TABS = 8
local clickedTab = nil
function SpellBookSkillLineTab_OnClick(self, id)
    clickedTab = id
    SpellBookFrame.selectedSkillLine = id
end
for i = 1, 8 do
    _G["SpellBookSkillLineTab" .. i] = {
        id = i,
        shown = (i <= 3), -- only 3 tabs shown (General, Spec 1, Spec 2)
        IsShown = function(self) return self.shown end,
        Click = function(self) SpellBookSkillLineTab_OnClick(self, self.id) end
    }
end
"""
        # Start at tab 1, press R1 -> tab 2
        test1 = setup + """
assert(runCycle("SpellBookFrame", 1) == true)
assert(SpellBookFrame.selectedSkillLine == 2)
-- Press R1 again -> tab 3
assert(runCycle("SpellBookFrame", 1) == true)
assert(SpellBookFrame.selectedSkillLine == 3)
-- Press R1 again -> wrap around to tab 1
assert(runCycle("SpellBookFrame", 1) == true)
assert(SpellBookFrame.selectedSkillLine == 1)
-- Press L1 -> wrap back to tab 3
assert(runCycle("SpellBookFrame", -1) == true)
assert(SpellBookFrame.selectedSkillLine == 3)
-- Press L1 again -> tab 2
assert(runCycle("SpellBookFrame", -1) == true)
assert(SpellBookFrame.selectedSkillLine == 2)
print("PASS")
"""
        self.assertIn("PASS", run_lua_test(test1))

    def test_spellbook_single_tab_no_cycle(self):
        setup = """
SpellBookFrame = { selectedSkillLine = 1 }
for i = 1, 8 do
    _G["SpellBookSkillLineTab" .. i] = {
        shown = (i == 1), -- only 1 tab shown
        IsShown = function(self) return self.shown end,
        Click = function(self) end
    }
end
local switched = runCycle("SpellBookFrame", 1)
assert(switched == false)
print("PASS")
"""
        self.assertIn("PASS", run_lua_test(setup))

    def test_character_frame_cycling_skip_hidden_pet(self):
        setup = """
CharacterFrame = { selectedTab = 1, numTabs = 5 }
function PanelTemplates_GetSelectedTab(f) return f.selectedTab end
function PanelTemplates_SetTab(f, n) f.selectedTab = n end
local subframe = nil
function CharacterFrame_ShowSubFrame(id) subframe = id end
function CharacterFrameTab_OnClick(self, button)
    CharacterFrame.selectedTab = self.id
    CharacterFrame_ShowSubFrame(self.id)
end

-- Tab 1: Character (shown)
-- Tab 2: Pet (HIDDEN - no pet active)
-- Tab 3: Reputation (shown)
-- Tab 4: Skill (shown)
-- Tab 5: Token (shown)
for i = 1, 5 do
    _G["CharacterFrameTab" .. i] = {
        id = i,
        shown = (i ~= 2),
        IsShown = function(self) return self.shown end,
        Click = function(self) CharacterFrameTab_OnClick(self, "LeftButton") end
    }
end

-- Start on tab 1 (Character). R1 -> skips 2 directly to 3 (Reputation)
assert(runCycle("CharacterFrame", 1) == true)
assert(CharacterFrame.selectedTab == 3)

-- R1 -> tab 4 (Skill)
assert(runCycle("CharacterFrame", 1) == true)
assert(CharacterFrame.selectedTab == 4)

-- R1 -> tab 5 (Token)
assert(runCycle("CharacterFrame", 1) == true)
assert(CharacterFrame.selectedTab == 5)

-- R1 -> wrap to tab 1 (Character)
assert(runCycle("CharacterFrame", 1) == true)
assert(CharacterFrame.selectedTab == 1)

-- L1 -> wrap back to tab 5 (Token)
assert(runCycle("CharacterFrame", -1) == true)
assert(CharacterFrame.selectedTab == 5)

-- L1 -> tab 4 -> tab 3 -> tab 1
assert(runCycle("CharacterFrame", -1) == true)
assert(CharacterFrame.selectedTab == 4)
assert(runCycle("CharacterFrame", -1) == true)
assert(CharacterFrame.selectedTab == 3)
assert(runCycle("CharacterFrame", -1) == true)
assert(CharacterFrame.selectedTab == 1)
print("PASS")
"""
        self.assertIn("PASS", run_lua_test(setup))

    def test_player_talent_frame_cycling(self):
        setup = """
PlayerTalentFrame = { selectedTab = 1, shown = true, IsShown = function(self) return self.shown end }
function PlayerTalentFrameTab_OnClick(self)
    PlayerTalentFrame.selectedTab = self.id
end
for i = 1, 3 do
    _G["PlayerTalentFrameTab" .. i] = {
        id = i,
        shown = true,
        IsShown = function(self) return self.shown end,
        Click = function(self) PlayerTalentFrameTab_OnClick(self) end
    }
end

-- Tab 1 -> Tab 2 -> Tab 3 -> Tab 1
assert(runCycle("PlayerTalentFrame", 1) == true)
assert(PlayerTalentFrame.selectedTab == 2)
assert(runCycle("PlayerTalentFrame", 1) == true)
assert(PlayerTalentFrame.selectedTab == 3)
assert(runCycle("PlayerTalentFrame", 1) == true)
assert(PlayerTalentFrame.selectedTab == 1)

-- Reverse L1: Tab 1 -> Tab 3 -> Tab 2 -> Tab 1
assert(runCycle("PlayerTalentFrame", -1) == true)
assert(PlayerTalentFrame.selectedTab == 3)
assert(runCycle("PlayerTalentFrame", -1) == true)
assert(PlayerTalentFrame.selectedTab == 2)
assert(runCycle("PlayerTalentFrame", -1) == true)
assert(PlayerTalentFrame.selectedTab == 1)
print("PASS")
"""
        self.assertIn("PASS", run_lua_test(setup))

    def test_trade_skill_and_craft_frame(self):
        setup = """
CraftFrame = { selectedTab = 1, numTabs = 2 }
for i = 1, 2 do
    _G["CraftFrameTab" .. i] = {
        id = i,
        shown = true,
        IsShown = function(self) return self.shown end,
        Click = function(self) CraftFrame.selectedTab = self.id end
    }
end
assert(runCycle("CraftFrame", 1) == true)
assert(CraftFrame.selectedTab == 2)
assert(runCycle("CraftFrame", 1) == true)
assert(CraftFrame.selectedTab == 1)

-- TradeSkillFrame without tabs returns false
TradeSkillFrame = { numTabs = 0 }
assert(runCycle("TradeSkillFrame", 1) == false)
print("PASS")
"""
        self.assertIn("PASS", run_lua_test(setup))

    def test_non_tabbed_panel_returns_false(self):
        setup = """
QuestLogFrame = {}
assert(runCycle("QuestLogFrame", 1) == false)
assert(runCycle("QuestLogFrame", -1) == false)
MerchantFrame = {}
assert(runCycle("MerchantFrame", 1) == false)
GossipFrame = {}
assert(runCycle("GossipFrame", 1) == false)
print("PASS")
"""
        self.assertIn("PASS", run_lua_test(setup))

    def test_robust_against_nil_frames(self):
        setup = """
assert(runCycle("NonExistentFrame", 1) == false)
assert(runCycle("SpellBookFrame", 1) == false) -- nil SpellBookSkillLineTabs
assert(runCycle("CharacterFrame", 1) == false)
assert(runCycle("PlayerTalentFrame", 1) == false)
print("PASS")
"""
        self.assertIn("PASS", run_lua_test(setup))

if __name__ == '__main__':
    unittest.main()
