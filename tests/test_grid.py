#!/usr/bin/env python

import os
import unittest
import gemmi

class TestFloatGrid(unittest.TestCase):
    def test_reading(self):
        path = os.path.join(os.path.dirname(__file__), '5i55_tiny.ccp4')
        m = gemmi.read_ccp4_map(path)
        self.assertEqual(m.grid.nu, 8)
        self.assertEqual(m.grid.nv, 6)
        self.assertEqual(m.grid.nw, 10)
        self.assertEqual(m.header_i32(28), 0)
        m.set_header_i32(28, 20140)  # set NVERSION
        self.assertEqual(m.header_i32(28), 20140)
        dmax = m.header_float(21)
        self.assertEqual(dmax, max(m.grid))
        m.setup()
        self.assertEqual(m.grid.nu, 60)
        self.assertEqual(m.grid.nv, 24)
        self.assertEqual(m.grid.nw, 60)
        self.assertEqual(m.header_float(14), 90.0)  # 14 - alpha angle
        self.assertEqual(m.grid.unit_cell.alpha, 90.0)

    def test_new(self):
        N = 24
        m = gemmi.FloatGrid(N, N, N)
        self.assertEqual(m.nu, N)
        self.assertEqual(m.nv, N)
        self.assertEqual(m.nw, N)
        m.set_value(1,2,3, 1.0)
        self.assertEqual(sum(m), 1.0)
        m.space_group = gemmi.find_spacegroup_by_name('C2')
        self.assertEqual(m.space_group.number, 5)
        m.symmetrize_max()
        self.assertEqual(sum(m), 4.0)
        m.fill(2.0)
        m.space_group = gemmi.find_spacegroup_by_name('P 62 2 2')
        self.assertEqual(len(m.space_group.operations()), 12)
        m.set_value(1, 2, 3, 0.0)
        m.symmetrize_min()
        self.assertEqual(sum(m), 2 * N * N * N - 2 * 12)


# In 5a11 applying NCS causes atom clashing
FRAGMENT_5A11 = """\
CRYST1   48.367   89.613   83.842  90.00 101.08  90.00 P 1 21 1      4          
MTRIX1   1 -0.999980  0.006670  0.000050       13.74419                         
MTRIX2   1  0.006260  0.941760 -0.336220       35.56956                         
MTRIX3   1 -0.002290 -0.336210 -0.941780      205.34927                         
ATOM    256  SG  CYS A  37      -1.002 -31.125  88.394  1.00 14.38           S  
ATOM   2969  SG  CYS B  37      14.582 -23.455 132.554  1.00 18.14           S  
"""  # noqa: W291 - trailing whitespace

# In 1gtv two different chains (with partial occupancy) are exactly in the
# same place
FRAGMENT_1GTV = """\
CRYST1   76.353   76.353  134.815  90.00  90.00 120.00 P 65 2 2     24
ATOM    635  SG  CYS A  85      42.948   6.483  17.913  0.48 23.86           S
ATOM   2293  SG  CYS B  85      42.948   6.483  17.913  0.52 23.86           S
"""

class TestSubCells(unittest.TestCase):
    def test_5a11(self):
        st = gemmi.read_pdb_string(FRAGMENT_5A11)
        a1 = st[0].sole_residue('A', 37, ' ')[0]
        sc = gemmi.SubCells(st[0], st.cell, 5)
        marks = sc.find_atoms(a1.pos, a1.altloc, 3)
        m1, m2 = sorted(marks, key=lambda m: sc.dist(a1.pos, m.pos()))
        self.assertAlmostEqual(sc.dist(a1.pos, m1.pos()), 0, delta=5e-6)
        self.assertAlmostEqual(sc.dist(a1.pos, m2.pos()), 0.13, delta=5e-3)
        cra2 = m2.to_cra(st[0])
        self.assertEqual(cra2.chain.name, 'B')
        self.assertEqual(str(cra2.residue.seqid), '37')
        self.assertEqual(cra2.atom.name, 'SG')

    def test_1gtv(self):
        st = gemmi.read_pdb_string(FRAGMENT_1GTV)
        a1 = st[0].sole_residue('A', 85, ' ')[0]
        subcells = gemmi.SubCells(st[0], st.cell, 5)
        marks = subcells.find_atoms(a1.pos, a1.altloc, 3)
        self.assertEqual(len(marks), 2)
        for mark in marks:
            d = subcells.dist(a1.pos, mark.pos())
            self.assertAlmostEqual(d, 0, delta=5e-6)

if __name__ == '__main__':
    unittest.main()
