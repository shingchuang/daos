//
// (C) Copyright 2019 Intel Corporation.
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
	"fmt"
	"net"
	"time"

	"github.com/pkg/errors"
	"golang.org/x/net/context"
	"google.golang.org/grpc"

	mgmtpb "github.com/daos-stack/daos/src/control/common/proto/mgmt"
	"github.com/daos-stack/daos/src/control/logging"
	"github.com/daos-stack/daos/src/control/security"
)

const (
	retryDelay = 3 * time.Second
)

type (
	// GrpcClientCfgHelper provides config access.
	GrpcClientCfgHelper interface {
		GetConfig() *grpcClientCfg
		SetConfig(grpcClientCfg)
		LeaderAddress() (string, error)
	}
	// grpcClientCfgHelper implements GrpcClientCfgHelper.
	grpcClientCfgHelper struct {
		cfg grpcClientCfg
	}
	// GrpcClient provides a subset of MgmtSvcClient for operations between gRPC servers.
	GrpcClient interface {
		GrpcClientCfgHelper
		Join(context.Context, *mgmtpb.JoinReq) (*mgmtpb.JoinResp, error)
		PrepShutdown(context.Context, string, mgmtpb.RanksReq) (*mgmtpb.RanksResp, error)
		Stop(context.Context, string, mgmtpb.RanksReq) (*mgmtpb.RanksResp, error)
		Start(context.Context, string, mgmtpb.RanksReq) (*mgmtpb.RanksResp, error)
		Query(context.Context, string, mgmtpb.RanksReq) (*mgmtpb.RanksResp, error)
	}
	// grpcClient implements GrpcClient.
	grpcClient struct {
		log logging.Logger
		*grpcClientCfgHelper
	}
	grpcClientCfg struct {
		AccessPoints    []string
		ControlAddr     *net.TCPAddr
		TransportConfig *security.TransportConfig
	}
)

func (gcch *grpcClientCfgHelper) GetConfig() *grpcClientCfg {
	return &gcch.cfg
}

func (gcch *grpcClientCfgHelper) SetConfig(cfg grpcClientCfg) {
	gcch.cfg = cfg
}

func (gcch *grpcClientCfgHelper) LeaderAddress() (string, error) {
	if len(gcch.cfg.AccessPoints) == 0 {
		return "", errors.New("no access points defined")
	}

	// TODO: Develop algorithm for determining current leader.
	// For now, just choose the first address.
	return gcch.cfg.AccessPoints[0], nil
}

func newGrpcClient(ctx context.Context, log logging.Logger, cfg grpcClientCfg) GrpcClient {
	return &grpcClient{
		log:                 log,
		grpcClientCfgHelper: &grpcClientCfgHelper{cfg: cfg},
	}
}

// delayRetry delays next retry.
func (gc *grpcClient) delayRetry(ctx context.Context) {
	select {
	case <-ctx.Done(): // break early if the parent context is canceled
	case <-time.After(retryDelay): // otherwise, block until after the delay duration
	}
}

func (gc *grpcClient) withConnection(ctx context.Context, ap string,
	fn func(context.Context, mgmtpb.MgmtSvcClient) error) error {

	var opts []grpc.DialOption
	authDialOption, err := security.DialOptionForTransportConfig(gc.cfg.TransportConfig)
	if err != nil {
		return errors.Wrap(err, "Failed to determine dial option from TransportConfig")
	}

	// Setup Dial Options that will always be included.
	opts = append(opts, grpc.WithBlock(), grpc.WithBackoffMaxDelay(retryDelay),
		grpc.WithDefaultCallOptions(grpc.FailFast(false)), authDialOption)
	conn, err := grpc.DialContext(ctx, ap, opts...)
	if err != nil {
		return errors.Wrapf(err, "dial %s", ap)
	}
	defer conn.Close()

	return fn(ctx, mgmtpb.NewMgmtSvcClient(conn))
}

func (gc *grpcClient) retryOnErr(err error, ctx context.Context, prefix string) bool {
	if err != nil {
		gc.log.Debugf("%s: %v", prefix, err)
		gc.delayRetry(ctx)
		return true
	}

	return false
}

func (gc *grpcClient) retryOnStatus(status int32, ctx context.Context, prefix string) bool {
	if status != 0 {
		gc.log.Debugf("%s: %d", prefix, status)
		gc.delayRetry(ctx)
		return true
	}

	return false
}

func (gc *grpcClient) Join(ctx context.Context, req *mgmtpb.JoinReq) (resp *mgmtpb.JoinResp, joinErr error) {
	ap, err := gc.LeaderAddress()
	if err != nil {
		return nil, err
	}

	joinErr = gc.withConnection(ctx, ap,
		func(ctx context.Context, pbClient mgmtpb.MgmtSvcClient) error {
			if req.Addr == "" {
				req.Addr = gc.cfg.ControlAddr.String()
			}

			prefix := fmt.Sprintf("join(%s, %+v)", ap, *req)
			gc.log.Debugf(prefix + " begin")
			defer gc.log.Debugf(prefix + " end")

			for {
				var err error

				select {
				case <-ctx.Done():
					return errors.Wrap(ctx.Err(), prefix)
				default:
				}

				resp, err = pbClient.Join(ctx, req)
				if gc.retryOnErr(err, ctx, prefix) {
					continue
				}
				if resp == nil {
					return errors.New("unexpected nil response status")
				}
				// TODO: Stop retrying upon certain errors (e.g., "not
				// MS", "rank unavailable", and "excluded").
				if gc.retryOnStatus(resp.Status, ctx, prefix) {
					continue
				}

				return nil
			}
		})

	return
}

// PrepShutdown calls function remotely over gRPC on server listening at destAddr.
//
// Shipped function propose ranks for shutdown by sending requests over dRPC
// to each rank.
func (gc *grpcClient) PrepShutdown(ctx context.Context, destAddr string, req mgmtpb.RanksReq) (resp *mgmtpb.RanksResp, psErr error) {
	psErr = gc.withConnection(ctx, destAddr,
		func(ctx context.Context, pbClient mgmtpb.MgmtSvcClient) (err error) {

			gc.log.Debugf("prep shutdown(%s, %+v)", destAddr, req)

			resp, err = pbClient.PrepShutdownRanks(ctx, &req)

			return
		})

	return
}

// Stop calls function remotely over gRPC on server listening at destAddr.
//
// Shipped function terminates ranks directly from the harness at the listening
// address without requesting over dRPC.
func (gc *grpcClient) Stop(ctx context.Context, destAddr string, req mgmtpb.RanksReq) (resp *mgmtpb.RanksResp, stopErr error) {
	stopErr = gc.withConnection(ctx, destAddr,
		func(ctx context.Context, pbClient mgmtpb.MgmtSvcClient) error {

			prefix := fmt.Sprintf("stop(%s, %+v)", destAddr, req)
			gc.log.Debugf(prefix + " begin")
			defer gc.log.Debugf(prefix + " end")

			for {
				var err error

				select {
				case <-ctx.Done():
					return errors.Wrap(ctx.Err(), prefix)
				default:
				}

				// returns on time out or when all instances are stopped
				// error returned if any instance is still running so that
				// we retry until all are terminated on host
				resp, err = pbClient.StopRanks(ctx, &req)
				if gc.retryOnErr(err, ctx, prefix) {
					continue
				}
				if resp == nil {
					return errors.New("unexpected nil response status")
				}
				// TODO: Stop retrying upon certain errors.

				return nil
			}
		})

	return
}

// Start calls function remotely over gRPC on server listening at destAddr.
//
// Shipped function issues StartRanks requests over dRPC to start each
// rank managed by the harness listening at the destination address.
//
// StartRanks will return results for any instances started by the harness.
func (gc *grpcClient) Start(ctx context.Context, destAddr string, req mgmtpb.RanksReq) (resp *mgmtpb.RanksResp, startErr error) {
	startErr = gc.withConnection(ctx, destAddr,
		func(ctx context.Context, pbClient mgmtpb.MgmtSvcClient) (err error) {

			prefix := fmt.Sprintf("start(%s, %+v)", destAddr, req)
			gc.log.Debugf(prefix + " begin")
			defer gc.log.Debugf(prefix + " end")

			ctx, _ = context.WithTimeout(ctx, retryDelay)

			// returns on time out or when all instances are running
			// don't retry
			resp, err = pbClient.StartRanks(ctx, &req)

			return
		})

	return
}

// Query calls function remotely over gRPC on server listening at destAddr.
//
// Shipped function issues PingRank dRPC requests to query each rank to verify
// activity.
//
// PingRanks should return ping results for any instances managed by the harness.
func (gc *grpcClient) Query(ctx context.Context, destAddr string, req mgmtpb.RanksReq) (resp *mgmtpb.RanksResp, statusErr error) {
	statusErr = gc.withConnection(ctx, destAddr,
		func(ctx context.Context, pbClient mgmtpb.MgmtSvcClient) (err error) {

			prefix := fmt.Sprintf("status(%s, %+v)", destAddr, req)
			gc.log.Debugf(prefix + " begin")
			defer gc.log.Debugf(prefix + " end")

			resp, err = pbClient.PingRanks(ctx, &req)

			return
		})

	return
}
