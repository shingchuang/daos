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

	mgmtpb "github.com/daos-stack/daos/src/control/common/proto/mgmt"
	"github.com/daos-stack/daos/src/control/logging"
)

type mockGrpcClient struct {
	log     logging.Logger
	retvals mockGrpcClientRetvals
	*grpcClientCfgHelper
}

type mockGrpcClientRetvals struct {
	startResp *mgmtpb.RanksResp
	startErr  error
}

func (mgc *mockGrpcClient) Join(ctx context.Context, req *mgmtpb.JoinReq) (resp *mgmtpb.JoinResp, joinErr error) {
	return
}

func (mgc *mockGrpcClient) PrepShutdown(ctx context.Context, destAddr string, req mgmtpb.RanksReq) (resp *mgmtpb.RanksResp, psErr error) {
	return
}

func (mgc *mockGrpcClient) Start(ctx context.Context, destAddr string, req mgmtpb.RanksReq) (resp *mgmtpb.RanksResp, psErr error) {
	return
}

func (mgc *mockGrpcClient) Stop(ctx context.Context, destAddr string, req mgmtpb.RanksReq) (resp *mgmtpb.RanksResp, psErr error) {
	return
}

func (mgc *mockGrpcClient) Query(ctx context.Context, destAddr string, req mgmtpb.RanksReq) (resp *mgmtpb.RanksResp, psErr error) {
	return
}

func newMockGrpcClient(log logging.Logger, cfg grpcClientCfg, rets mockGrpcClientRetvals) GrpcClient {
	return &mockGrpcClient{
		log:                 log,
		grpcClientCfgHelper: &grpcClientCfgHelper{cfg},
		retvals:             rets,
	}
}
