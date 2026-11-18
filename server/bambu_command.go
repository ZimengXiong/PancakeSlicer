package main

import (
	"context"
	"errors"
	"log"
	"net/http"
	"strings"
)

type CommandState string

const (
	CommandPending     CommandState = "pending"
	CommandConfirmed   CommandState = "confirmed"
	CommandRejected    CommandState = "rejected"
	CommandUnconfirmed CommandState = "unconfirmed"
)

var commandTransitions = map[CommandState]map[CommandState]bool{
	CommandPending:     {CommandConfirmed: true, CommandRejected: true, CommandUnconfirmed: true},
	CommandConfirmed:   {},
	CommandRejected:    {},
	CommandUnconfirmed: {},
}

type bambuCommand struct {
	scope       string
	body        map[string]any
	codes, what string
	judge       func(r bambuHeard, accepted bool) (CommandState, string, []any)
}

type commandRun struct {
	printer, what string
	state         CommandState
	failure       failure
	accepted      bool
}

func (c *commandRun) to(next CommandState, f failure) bool {
	if !commandTransitions[c.state][next] {
		log.Printf("command %s %s: illegal transition %s -> %s", c.printer, c.what, c.state, next)
		return false
	}
	c.state, c.failure = next, f
	return true
}

type commandResult struct {
	State CommandState `json:"state,omitempty"`
	Error string       `json:"error,omitempty"`
	Code  string       `json:"code,omitempty"`
	Step  string       `json:"step,omitempty"`
	Trays []AMSTray    `json:"trays,omitempty"`
}

func (r commandResult) status() int {
	if r.State == CommandConfirmed {
		return http.StatusOK
	}
	return http.StatusBadGateway
}

func (s *Server) runBambuCommand(ctx context.Context, p *PrinterConfig, secret string, c bambuCommand) commandResult {
	reports, stop := s.sends.hub.listen(p.Printer)
	defer stop()
	sequence, echoes, closeSession, err := bambuPublish(ctx, p, secret, c.scope, c.body)
	if err != nil {
		var conn *connError
		code := c.codes + "_not_sent"
		if errors.As(err, &conn) {
			code += "_" + conn.stage
		}
		f := newFailure(code, redact(err.Error(), secret))
		return commandResult{Error: f.Error, Code: f.Code, Step: f.Step}
	}
	defer closeSession()
	action, _ := c.body["command"].(string)
	run := &commandRun{printer: p.Printer, what: c.what, state: CommandPending}
	standing := func() {
		if st, ok := s.sends.hub.current(p.Printer); ok {
			if state, _, _ := c.judge(bambuHeard{status: &st, trays: true, state: st.Trays}, run.accepted); state == CommandConfirmed {
				run.to(CommandConfirmed, failure{})
			}
		}
	}
	standing()
	echo := func(e bambuEcho) {
		if e.ErrCode != 0 || e.Result != "" && !strings.EqualFold(e.Result, "success") {
			refusal := newFailure(c.codes+"_rejected", p.Name, c.what, cmpOr(e.Reason, e.Result), e.ErrCode)
			if refusedForAuth(e.Result, e.Reason, e.ErrCode) {
				refusal = newFailure(c.codes+"_developer_mode", p.Name, c.what)
			}
			run.to(CommandRejected, refusal)
			return
		}
		if run.accepted {
			return
		}
		run.accepted = true
		standing()
	}
	for run.state == CommandPending {
		select {
		case e, open := <-echoes:
			if !open {
				echoes = nil
				continue
			}
			echo(e)
		case rep := <-reports:
			if rep.echo != nil && rep.echo.Command == action && rep.echo.Sequence == sequence {
				echo(*rep.echo)
			}
			if run.state == CommandPending {
				if state, code, args := c.judge(rep, run.accepted); state != CommandPending {
					run.to(state, newFailure(code, args...))
				}
			}
		case <-ctx.Done():
			run.to(CommandUnconfirmed, newFailure(c.codes+"_unconfirmed", p.Name, c.what))
		}
	}
	return commandResult{State: run.state, Error: run.failure.Error, Code: run.failure.Code, Step: run.failure.Step}
}

func shownBy(shows func(PrinterStatus) bool) func(bambuHeard, bool) (CommandState, string, []any) {
	return func(r bambuHeard, _ bool) (CommandState, string, []any) {
		if r.status != nil && shows(*r.status) {
			return CommandConfirmed, "", nil
		}
		return CommandPending, "", nil
	}
}
