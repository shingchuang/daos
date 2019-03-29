/**
 * (C) Copyright 2017-2019 Intel Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * GOVERNMENT LICENSE RIGHTS-OPEN SOURCE SOFTWARE
 * The Government's rights to use, modify, reproduce, release, perform, display,
 * or disclose this software are subject to the terms of the Apache License as
 * provided in Contract No. B609815.
 * Any reproduction of computer software, computer software documentation, or
 * portions thereof marked with this legend must also reproduce the markings.
 */

#include "dfuse_log.h"

#include <cart/api.h>

#include "dfuse_fs.h"

static int
iof_check_complete(void *arg)
{
	struct iof_tracker *tracker = arg;

	return iof_tracker_test(tracker);
}

/* Progress until all callbacks are invoked */
void
iof_wait(crt_context_t crt_ctx, struct iof_tracker *tracker)
{
	int			rc;

	for (;;) {
		rc = crt_progress(crt_ctx, 1000 * 1000, iof_check_complete,
				  tracker);

		if (iof_tracker_test(tracker))
			return;

		/* TODO: Determine the best course of action on error.  In an
		 * audit of cart code, it seems like this would only happen
		 * under somewhat catostrophic circumstances.
		 */
		if (rc != 0 && rc != -DER_TIMEDOUT)
			IOF_LOG_ERROR("crt_progress failed rc: %d", rc);
	}
}