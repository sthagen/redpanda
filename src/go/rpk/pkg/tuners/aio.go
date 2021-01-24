// Copyright 2020 Vectorized, Inc.
//
// Use of this software is governed by the Business Source License
// included in the file licenses/BSL.md
//
// As of the Change Date specified in that file, in accordance with
// the Business Source License, use of this software will be governed
// by the Apache License, Version 2.0

package tuners

import (
	"fmt"

	log "github.com/sirupsen/logrus"
	"github.com/spf13/afero"
	"github.com/vectorizedio/redpanda/src/go/rpk/pkg/tuners/executors"
	"github.com/vectorizedio/redpanda/src/go/rpk/pkg/tuners/executors/commands"
	"github.com/vectorizedio/redpanda/src/go/rpk/pkg/utils"
)

const maxAIOEvents = 1048576
const maxAIOEventsFile = "/proc/sys/fs/aio-max-nr"

func NewMaxAIOEventsChecker(fs afero.Fs) Checker {
	return NewIntChecker(
		MaxAIOEvents,
		"Max AIO Events",
		Warning,
		func(current int) bool {
			return current >= maxAIOEvents
		},
		func() string {
			return fmt.Sprintf(">= %d", maxAIOEvents)
		},
		func() (int, error) {
			return utils.ReadIntFromFile(fs, maxAIOEventsFile)
		},
	)
}

func NewMaxAIOEventsTuner(fs afero.Fs, executor executors.Executor) Tunable {
	return NewCheckedTunable(
		NewMaxAIOEventsChecker(fs),
		func() TuneResult {
			log.Debugf("Setting max AIO events to %d", maxAIOEvents)
			err := executor.Execute(
				commands.NewWriteFileCmd(
					fs,
					maxAIOEventsFile,
					fmt.Sprint(maxAIOEvents),
				),
			)
			if err != nil {
				return NewTuneError(err)
			}
			return NewTuneResult(false)
		},
		func() (bool, string) {
			return true, ""
		},
		executor.IsLazy(),
	)
}
