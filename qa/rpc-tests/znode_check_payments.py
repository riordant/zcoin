#!/usr/bin/env python3
import time
from decimal import *

from test_framework.test_framework import ZnodeTestFramework
from test_framework.util import *

# Description: a very straightforward check of Znode operability
# 1. start Znodes
# 2. mine blocks
# 3. check znode reward
class ZnodeCheckPayments(ZnodeTestFramework):
    def __init__(self):
        super().__init__()
        self.num_nodes = 4
        self.setup_clean_chain = True

    def run_test(self):
        self.generate_znode_collateral()
        sync_blocks(self.nodes)

        collateral = self.send_znode_collateral(1)

        self.nodes[0].generate(10)
        sync_blocks(self.nodes)

        self.generate_znode_privkey(1, 1)
        self.write_master_znode_conf(1, self.znode_priv_keys[1], collateral.tx_id, collateral.n, 1)
        self.restart_as_znode(1)

        wait_to_sync_znodes(self.nodes[0])
        wait_to_sync_znodes(self.nodes[1])
        wait_to_sync_znodes(self.nodes[3])


        print(self.nodes[1].znode("start"))
        print(self.nodes[1].znode("status"))
        print(self.nodes[1].znode("debug"))
        print(self.nodes[0].znode("count", "all"))
        print(self.nodes[1].znode("count", "all"))
        print(self.nodes[3].znode("count", "all"))

        for i in range(10):
            self.nodes[0].generate(1)
            time.sleep(10)

        wait_to_sync_znodes(self.nodes[0])
        wait_to_sync_znodes(self.nodes[1])
        wait_to_sync_znodes(self.nodes[3])

        print(self.nodes[0].znsync("status"))
        print(self.nodes[1].znsync("status"))
        print(self.nodes[3].znsync("status"))

        print(self.nodes[1].znode("start"))
        for i in range(10):
            self.nodes[0].generate(1)
            time.sleep(10)

        print(self.nodes[1].znode("status"))

        print(self.nodes[0].znode("count", "all"))
        print(self.nodes[1].znode("count", "all"))
        print(self.nodes[3].znode("count", "all"))
        print(self.nodes[1].znode("status"))

        assert_equal(1, 0)

        print(self.nodes[1].getinfo());
        self.nodes[3].generate(10)
        sync_blocks(self.nodes)
        print(self.nodes[1].getinfo());

        assert_equal (1, 0)


if __name__ == '__main__':
    ZnodeCheckPayments().main()