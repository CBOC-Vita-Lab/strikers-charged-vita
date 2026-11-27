#!/usr/bin/env python3
"""ImpostorManager::UpdateSpritesWithFloor's slot arithmetic (copied here as a model, the class does not link on a host): with a floor every sprite still gets its turn, the
throttle other callers see is unchanged, and a caller without a floor (the front end) starts inside its own period, so that it captures on the frame it asks."""


class M:
    def __init__(self):
        self.slot = self.floor_slot = 0
        self.period = 2
        self.throttle = 1

    def plain(self, sprites):
        if self.floor_slot:
            self.floor_slot = 0
            self.slot %= self.period * self.throttle
        p = self.period * self.throttle
        hit = [s for s in sprites if s % p == self.slot]
        self.slot = (self.slot + 1) % p
        return hit

    def floored(self, floor, sprites):
        was = self.throttle
        if floor > was:
            self.throttle = min(4, floor)
        self.floor_slot = 0
        hit = self.plain(sprites)
        self.floor_slot = floor > was
        self.throttle = was
        return hit


sprites = list(range(32))
for period in (1, 2):
    for floor in (1, 2, 3, 4):
        for fg in (1, 2):
            m = M()
            m.period, m.throttle = period, fg
            n = period * max(fg, floor)
            seen = []
            for _ in range(n):
                seen += m.floored(floor, sprites)
            assert sorted(seen) == sprites, (period, floor, fg)
            assert m.throttle == fg
            for _ in range(5):
                m.floored(floor, sprites)
            hit = m.plain(sprites)
            assert hit, (period, floor, fg)   # the front end's frame captures
print("crowd schedule model: ok")
