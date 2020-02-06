//
// (C) Copyright 2020 Intel Corporation.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// GOVERNMENT LICENSE RIGHTS-OPEN SOURCE SOFTWARE
// The Government's rights to use, modify, reproduce, release, perform, display,
// or disclose this software are subject to the terms of the Apache License as
// provided in Contract No. 8F-30005.
// Any reproduction of computer software, computer software documentation, or
// portions thereof marked with this legend must also reproduce the markings.
//

package server

import (
	"context"
	"testing"

	"github.com/google/go-cmp/cmp"

	"github.com/daos-stack/daos/src/control/common"
	mgmtpb "github.com/daos-stack/daos/src/control/common/proto/mgmt"
	"github.com/daos-stack/daos/src/control/logging"
	"github.com/daos-stack/daos/src/control/server/ioserver"
	"github.com/daos-stack/daos/src/control/system"
)

const defaultAP = "192.168.1.1:10001"

func TestServer_HarnessClientStart(t *testing.T) {
	for name, tc := range map[string]struct {
		hAddr      string
		ranks      []uint32
		startResp  *mgmtpb.RanksResp
		startErr   error
		expResults system.MemberResults
		expErr     error
	}{
		"single rank success": {
			hAddr:      "localhost",
			expResults: system.MemberResults{},
			// expErr:     errors.New("HarnessClient request: no access points defined"),
		},
	} {
		t.Run(name, func(t *testing.T) {
			log, buf := logging.NewTestLogger(t.Name())
			defer common.ShowBufferOnFailure(t, buf)

			ioserverCount := maxIoServers
			svc := newTestMgmtSvcMulti(log, ioserverCount, false)

			svc.harness.setStarted()
			svc.harness.setRestartable()

			clientCfg := grpcClientCfg{
				AccessPoints: []string{defaultAP},
			}
			clientRets := mockGrpcClientRetvals{
				startResp: tc.startResp,
				startErr:  tc.startErr,
			}
			for i, srv := range svc.harness.instances {
				srv._superblock.Rank = new(ioserver.Rank)
				*srv._superblock.Rank = ioserver.Rank(i + 1)
				if i == 0 {
					srv._superblock.MS = true
					srv.msClient = newMockGrpcClient(log, clientCfg, clientRets)
				}
			}

			hc := NewHarnessClient(log, svc.harness)

			gotResults, gotErr := hc.Start(context.TODO(), tc.hAddr)
			common.CmpErr(t, tc.expErr, gotErr)
			if tc.expErr != nil {
				return
			}

			if diff := cmp.Diff(tc.expResults, gotResults, common.DefaultCmpOpts()...); diff != "" {
				t.Fatalf("unexpected results (-want, +got)\n%s\n", diff)
			}
		})
	}
}
