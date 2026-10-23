#!/usr/bin/env python3
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
"""Check accounting used by the recursive-cache investigation."""

import unittest

from recursive_bench import counters, occupancy


class Accounting(unittest.TestCase):
    def test_recursive_statistics_are_summed_without_counting_regen_as_eval(self):
        log = ("*kati*: eval time: 0.125000\n"
               "*kati*: parse file time: 0.100000 / 3\n"
               "*kati*: parse eval time: 0.005000 / 8\n"
               "*kati*: eval time: 0.250000\n"
               "*kati*: parse file time: 0.200000 / 2\n"
               "No need to regenerate ninja file\n")
        result = counters(log)
        self.assertEqual(result["evaluation_passes"], 2)
        self.assertEqual(result["parsed_files"], 5)
        self.assertAlmostEqual(result["parse_seconds"], 0.3)
        self.assertEqual(result["evaluation_seconds"], 0.375)
        self.assertEqual(result["graph_reuses"], 1)

    def test_slot_occupancy_counts_overlap_and_boundary_ties(self):
        result = occupancy([(0, 2), (1, 3), (3, 4)], elapsed=5, jobs=4)
        self.assertEqual(result["peak_active_recipes"], 2)
        self.assertEqual(result["recipe_seconds"], 5)
        self.assertEqual(result["recipe_slot_occupancy"], 0.25)
        self.assertEqual(occupancy([], 1, 4)["peak_active_recipes"], 0)


if __name__ == "__main__":
    unittest.main()
