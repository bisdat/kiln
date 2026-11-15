# SPDX-License-Identifier: MIT
"""Offline tests only. No socket, subprocess, game executable or lab is opened."""
import unittest
from run import valid_result, rejected_before_stream

class ProofTests(unittest.TestCase):
    def test_required_passes_and_fresh_done_are_both_needed(self):
        r={'mode':'served','failed':0,'hash':'0123456789abcdef','passed':['membership']}
        good='mp: PASS membership\nmp: DONE served 1 passed 0 failed hash 0123456789abcdef'
        self.assertTrue(valid_result('served',r,good,['membership']))
        self.assertFalse(valid_result('restart',r,good,['membership']))
        self.assertFalse(valid_result('served',r,good,['binding']))
        self.assertFalse(valid_result('served',r,good.replace('PASS membership','FAIL membership'),['membership']))
    def test_no_done_or_wrong_hash_cannot_pass(self):
        r={'mode':'served','failed':0,'hash':'0123456789abcdef','passed':[]}
        self.assertFalse(valid_result('served',r,'',[]))
        self.assertFalse(valid_result('served',r,'mp: DONE served 0 passed 0 failed hash 1111111111111111',[]))
    def test_any_failure_overrides_pass_lines(self):
        r={'mode':'served','failed':1,'hash':'0123456789abcdef','passed':['membership']}
        self.assertFalse(valid_result('served',r,'mp: PASS membership\nmp: DONE served 1 passed 0 failed hash 0123456789abcdef',['membership']))
    def test_refusal_requires_table_reason_without_welcome_or_stream(self):
        reject=('building materials differ',[])
        self.assertTrue(rejected_before_stream(None,reject,0))
        self.assertFalse(rejected_before_stream({},reject,0))
        self.assertFalse(rejected_before_stream(None,reject,1))
        self.assertFalse(rejected_before_stream(None,('mods missing',[]),0))
        self.assertFalse(rejected_before_stream(None,None,0))
if __name__=='__main__':unittest.main()
