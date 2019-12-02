#!/usr/bin/python
"""
  (C) Copyright 2018-2019 Intel Corporation.

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

     http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

  GOVERNMENT LICENSE RIGHTS-OPEN SOURCE SOFTWARE
  The Government's rights to use, modify, reproduce, release, perform, display,
  or disclose this software are subject to the terms of the Apache License as
  provided in Contract No. B609815.
  Any reproduction of computer software, computer software documentation, or
  portions thereof marked with this legend must also reproduce the markings.
"""

from ior_test_base import IorTestBase


class IorIntercept(IorTestBase):
    """Test class Description: Runs IOR with and without interception
       library on a multi server and multi client settings with
       basic parameters.

    :avocado: recursive
    """

    def test_ior_intercept(self):
        """Jira ID: DAOS-3498.

        Test Description:
            Purpose of this test is to run ior using dfuse for 5 minutes
            and capture the metrics and use the intercepiton library by
            exporting LD_PRELOAD to the libioil.so path and rerun the
            above ior and capture the metrics and compare the
            performance difference and check using interception
            library make significant performance improvement.

        Use case:
            Run ior with read, write, CheckWrite, CheckRead, fsync
                in fpp mode for 5 minutes
            Run ior with read, write, CheckWrite, CheckRead, fsync
                in fpp mode for 5 minutes with interception library
            Compare the results and check whether using interception
                library provides better performance.

        :avocado: tags=all,daosio,hw,iorintercept
        """
        out = self.run_ior_with_pool()
        print("*****************************"
        print("*****************************"
        print("*****************************"
        print(self.prefix)
        print(out)
        print("*****************************"
        print("*****************************"
        print("*****************************"
