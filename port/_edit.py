p = 'src/fighter.cpp'
s = open(p).read()
old = '''        if (st == 1 && (f.stamina == 0 || f.in.held[0] == 0 || f.in.held[1] == 0)) {
            set_step(f, 2);
            return;
        }'''
new = '''        if (st == 1 && (f.stamina == 0 || f.in.held[0] == 0 || f.in.held[1] == 0)) {
            set_step(f, 2);
            return;
        }
        if (st == 1 && f.frame + 1 >= step_len(f)) f.frame = 0;  // the held guard repeats its step'''
assert old in s
s = s.replace(old, new, 1)
old = '''        if (a.state < 300 || a.step != int(m->steps.size()) - 1'''
new = '''        if (a.state < 300 || (a.state >= 390 && a.state <= 394) || a.step != int(m->steps.size()) - 1'''
assert old in s
s = s.replace(old, new, 1)
open(p, 'w').write(s)

p = 'tests/battle_test.cpp'
s = open(p).read()
old = '''        std::printf("%d: A%d D%d st%d hp%d sta%d\n", i, bt.f[0].state, bt.f[1].state, bt.f[1].step, bt.f[1].hp,
                    bt.f[1].stamina);
'''
assert old in s
s = s.replace(old, '''        if (bt.f[0].state == 392) recoiled = true;
''', 1)
s = s.replace('''    int blocked = 0;
    for (int i = 0; i < 60; ++i) {''', '''    int blocked = 0;
    bool recoiled = false;
    for (int i = 0; i < 60; ++i) {''', 1)
s = s.replace('''    check(blocked > 0 && bt.f[1].hp == 10000 && bt.f[1].stamina < 1000, "hit blocked: stamina, not HP");''', '''    check(blocked > 0 && bt.f[1].hp == 10000, "hit blocked: no HP lost");
    check(recoiled, "the guarded attacker recoils (392)");
    check(bt.f[1].stamina < 1000, "holding the guard costs stamina");''', 1)
s = s.replace('''    // Guard: jump + melee held together raises the guard (390); a hit from the front is
    // blocked (391): no HP lost, stamina spent; letting go lowers it.''', '''    // Guard: jump + melee held together raises the guard (390, 10 stamina a frame); a hit from
    // the front is blocked (391) and the attacker recoils (392); letting go lowers it.''', 1)
s = s.replace('''        {391, 0, 150, {{10, {{0, 0x400}}}}},
        {392, 0, 150, {{10, {shut}}}},''', '''        {391, 0, 150, {{10, {shut}}, {10, {shut}}}},
        {392, 0, 150, {{10, {shut}}, {10, {shut}}}},''', 1)
open(p, 'w').write(s)
