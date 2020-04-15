#!/usr/bin/env python3

import re, yaml
from types import SimpleNamespace
from typing import Dict

class SubscriptableNamespace(SimpleNamespace):
    def __init__(self, **kwargs):
        super().__init__(**kwargs)
    def __contains__(self, key):
        return key in self.__dict__
    def __getitem__(self, key):
        return self.__dict__[key]
    def __setitem__(self, key, value):
         self.__dict__[key] = value
    def __delitem__(self, key):
        del self.__dict__[key]
    # SimpleNamespace has value equality and is unhashable.  Identity equality is enough for
    # our purposes -- we'd check MBIDs/etc. if we care about value.
    def __hash__(self):
        return id(self)
    def __eq__(self, other):
        return self is other
    def copy(self):
        """Returns a shallow copy of this object."""
        x = SubscriptableNamespace()
        x.update(self)
        return x
    def update(self, other):
        self.__dict__.update(other.__dict__)


SQUARE_SPEC = re.compile(r'^square \((\d+), (\d+)\)$')
LINE_SPEC = re.compile(r'^line \((\d+), ?(\d+)\) \((\d+), ?(\d+)\)$')
RECTANGLE_SPEC = re.compile(r'^rectangle \((\d+), ?(\d+)\) (\d+) (\d+)$')

def parse_coord_spec(s):
    match = SQUARE_SPEC.match(s)
    if match:
        return {(int(match.group(1)), int(match.group(2)))}
    match = LINE_SPEC.match(s)
    if match:
        r, c = int(match.group(1)), int(match.group(2))
        mr, mc = int(match.group(3)), int(match.group(4))
        ret = set()
        if r == mr:
            return {(r, q) for q in range(c, mc+1)}
        elif c == mc:
            return {(q, c) for q in range(r, mr+1)}
        else:
            raise Exception('parse_coord_spec: not a line: '+s)
    match = RECTANGLE_SPEC.match(s)
    if match:
        r, c = int(match.group(1)), int(match.group(2))
        nr, nc = int(match.group(3)), int(match.group(4))
        return {(p, q) for p in range(r, r+nr) for q in range(c, c+nc)}
    raise Exception('parse_coord_spec: unknown: '+s)

RAIL_SPEC = re.compile(r'^rail (up|down|left|right) (.+)$')

def parse_area(cmds, allow_rails=True):
    area, rails = set(), set()
    for c in cmds:
        if c.startswith('add '):
            area.update(parse_coord_spec(c[4:]))
        elif c.startswith('remove '):
            # TODO: ensure we removed at least one thing, or all things?
            area.difference_update(parse_coord_spec(c[7:]))
        else:
            match = RAIL_SPEC.match(c)
            if match:
                coords = parse_coord_spec(match.group(2))
                if not coords & area: #TODO: more helpful error message
                    raise Exception('parse_area: assigning rail to missing coords: '+c)
                #TODO: duplicate rails
                rails.update([(coord, match.group(1)) for coord in coords])
            else:
                raise Exception('parse_area: unknown: '+c)
    if rails and not allow_rails:
        raise Exception('parse_area: rails provided when not allowed')
    return (frozenset(area), frozenset(rails)) if allow_rails else frozenset(area)

def up(c):
    return (c[0]-1, c[1])
def down(c):
    return (c[0]+1, c[1])
def left(c):
    return (c[0], c[1]-1)
def right(c):
    return (c[0], c[1]+1)

def partition_anchorable(area, rails):
    anchorable, unanchorable = [], []
    for c in area:
        if ((c, 'up') not in rails and up(c) in area) and ((c, 'down') not in rails and down(c) in area):
            anchorable.append(c)
        elif ((c, 'left') not in rails and left(c) in area) and ((c, 'right') not in rails and right(c) in area):
            anchorable.append(c)
        else:
            unanchorable.append(c)
    return (sorted(anchorable), sorted(unanchorable))

class Topology(object):
    def __init__(self, board_name, area, rails):
        self.name = board_name+"_topo"
        self.anchorable, self.unanchorable = partition_anchorable(area, rails)
        self.index_map = {coord: index for (index, coord) in enumerate(self.anchorable + self.unanchorable)}
        self.data = []
        for index, coord in enumerate(self.anchorable + self.unanchorable):
            for dir in ('left', 'up', 'right', 'down'):
                if (coord, dir) in rails:
                    self.data.append("RAIL")
                else:
                    next = globals()[dir](coord)
                    self.data.append(self.index_map.get(next, "VOID"))

class Placement(object):
    def __init__(self, board_name, index, placement, index_map):
        self.name = board_name+"_placement"+str(index)
        self.data = sorted(map(index_map.get, placement))




with open('boards.yaml', 'r') as f:
    data = yaml.load(f)

with open('src/board-defs.inc', 'w') as f:
    topology_cache = {}
    placement_cache = {}
    boards: Dict[str, SubscriptableNamespace] = {}

    for raw_board in data:
        board = SubscriptableNamespace()
        if 'inherits' in raw_board:
            inherits = raw_board['inherits']
            if isinstance(inherits, str):
                board.update(boards[inherits])
            elif isinstance(inherits, list):
                for i in inherits:
                    board.update(boards[i])
            else:
                raise ValueError('unrecognized inherits: ' + raw_board['inherits'])

        if raw_board['name'] in boards:
            raise ValueError('duplicate board name ' + raw_board['name'])
        board.name = raw_board['name']
        boards[board.name] = board

        COPYABLE_KEYS = ('pawns', 'pushers')
        for k in COPYABLE_KEYS:
            if k in raw_board:
                board[k] = raw_board[k]

        if 'topology' in raw_board:
            area, rails = parse_area(raw_board['topology'])
            topology = topology_cache.get((area, rails))
            if not topology:
                topology = Topology(board.name, area, rails)
                topology_cache[(area, rails)] = topology
                f.write('constexpr unsigned int {}[] = {{\n'.format(topology.name))
                for i in range(0, len(topology.data), 4):
                    f.write('\t')
                    f.write(' '.join([str(q) + ',' for q in topology.data[i:i + 4]]))
                    f.write('\n')
                f.write('};\n\n')
            board.topology = topology

        if 'placement' in raw_board:
            placement_areas = [parse_area(p, allow_rails=False) for p in raw_board['placement']]
            if len(placement_areas) != 2:
                raise Exception('too many placements for {}: {}'.format(board.name, len(placement_areas)))
            if len(placement_areas[0]) < board.pawns + board.pushers or len(placement_areas[1]) < board.pawns + board.pushers:
                raise Exception('placement area too small ' + raw_board)
            board.placements = []
            for i, p in enumerate(placement_areas):
                # dicts aren't hashable, so we have to do the mapping before looking up
                placement_indices = tuple(sorted(map(topology.index_map.get, p)))
                placement = placement_cache.get(placement_indices)
                if not placement:
                    placement = Placement(board.name, i, p, topology.index_map)
                    placement_cache[placement_indices] = placement
                    f.write('constexpr unsigned int {}[] = {{'.format(placement.name))
                    f.write(', '.join([str(q) for q in placement.data]))
                    f.write('};\n\n')
                board.placements.append(placement)

    for board in boards.values():
        f.write('const Board {}{{{}, {}, {}, {}, {}, {}, {}, {}, {}, {}}};\n\n'.format(
            board.name, '"{}"'.format(board.name),
            len(board.topology.anchorable) + len(board.topology.unanchorable),
            len(board.topology.anchorable),
            board.pushers, board.pawns,
            board.topology.name,
            board.placements[0].name, len(board.placements[0].data),
            board.placements[1].name, len(board.placements[1].data)))

