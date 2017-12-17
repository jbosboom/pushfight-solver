#!/usr/bin/env python2

import re, yaml

SQUARE_SPEC = re.compile(r'^square \((\d+), (\d+)\)$')
LINE_SPEC = re.compile(r'^line \((\d+), ?(\d+)\) \((\d+), ?(\d+)\)$')
RECTANGLE_SPEC = re.compile(r'^rectangle \((\d+), ?(\d+)\) (\d+) (\d+)$')

def parse_coord_spec(s):
  match = SQUARE_SPEC.match(s)
  if match:
    return set((int(match.group(1)), int(match.group(2))))
  match = LINE_SPEC.match(s)
  if match:
    r, c = int(match.group(1)), int(match.group(2))
    mr, mc = int(match.group(3)), int(match.group(4))
    ret = set()
    if r == mr:
      return {(r, q) for q in xrange(c, mc+1)}
    elif c == mc:
      return {(q, c) for q in xrange(r, mr+1)}
    else:
      raise Exception('parse_coord_spec: not a line: '+s)
  match = RECTANGLE_SPEC.match(s)
  if match:
    r, c = int(match.group(1)), int(match.group(2))
    nr, nc = int(match.group(3)), int(match.group(4))
    return {(p, q) for p in xrange(r, r+nr) for q in xrange(c, c+nc)}
  raise Exception('parse_coord_spec: unknown: '+s)

RAIL_SPEC = re.compile(r'^rail (up|down|left|right) (.+)$')

def parse_area(cmds):
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
  return (area, rails)

data = yaml.load(open('boards.yaml', 'r'))
print parse_area(data[0]['topology'])
