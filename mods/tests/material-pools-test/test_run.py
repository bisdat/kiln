# SPDX-License-Identifier: MIT
"""Offline tests only. No socket, subprocess, game executable or lab is opened."""
import unittest
from run import valid_result, rejected_before_stream, pool_digest, pools_property

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
    def test_refusal_names_the_pool_when_asked(self):
        reject=("This server's building and terrain materials don't match your game's: the material pool mp-fixture:four differs",[])
        self.assertTrue(rejected_before_stream(None,reject,0,'the material pool mp-fixture:four differs'))
        self.assertFalse(rejected_before_stream(None,reject,0,'the material pool mp-fixture:shared differs'))
    def test_pool_digest_matches_kiln_network(self):
        # the same inputs as Kiln.Network.Tests' APoolDigestFollowsOrderRoofAndBinding: order, roof and binding all count
        g='d484d837-f2d9-5cef-a617-196245f5db4c'
        one=pool_digest(g,False,[1,2],[])
        self.assertEqual(one,pool_digest(g,False,[1,2],[]))
        self.assertNotEqual(one,pool_digest(g,False,[2,1],[]))
        self.assertNotEqual(one,pool_digest(g,True,[1,2],[]))
        self.assertNotEqual(one,pool_digest(g,False,[1,2],['41a460a5-d463-5c2d-bf26-1a072d6e84b1']))
        self.assertEqual(pool_digest(g,False,[1,2],['41a460a5-d463-5c2d-bf26-1a072d6e84b1']),'1eb0229c3b458816')  # the C# test's value
        status={'customPools':[{'name':'m:p','guid':g,'roof':False,'items':[1,2]}],'shapeBindings':[{'pool':g,'item':'x'}]}
        self.assertEqual(pools_property(status),'m:p='+pool_digest(g,False,[1,2],['x']))
if __name__=='__main__':unittest.main()
