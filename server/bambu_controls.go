package main

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"net/http"
	"time"
)

type BambuControl struct {
	Index  int  `json:"index"`
	Min    int  `json:"min"`
	Max    int  `json:"max"`
	Modern bool `json:"modern,omitempty"`
}

type BambuControls struct {
	Fans    map[string]BambuControl `json:"fans,omitempty"`
	Chamber *BambuControl           `json:"chamber,omitempty"`
}

func bambuFanName(index int) string {
	return map[int]string{1: "part", 2: "auxiliary", 3: "chamber", 6: "internal", 10: "left auxiliary"}[index]
}

func (b *bambuReport) controls() *BambuControls {
	c := &BambuControls{Fans: map[string]BambuControl{}}
	if b.ChamberEditable {
		low, high := 0, 60
		if len(b.ChamberRange) >= 2 {
			low, high = b.ChamberRange[0], b.ChamberRange[1]
		}
		c.Chamber = &BambuControl{Min: low, Max: high}
	}
	if b.Device != nil && b.Device.Airduct != nil {
		a := b.Device.Airduct
		allowed := map[int]bool{}
		for _, mode := range a.ModeList {
			if mode.ID == a.ModeCur {
				for _, id := range mode.Control {
					allowed[id>>4] = true
				}
			}
		}
		for _, part := range a.Parts {
			index := part.ID >> 4 & 255
			if name := bambuFanName(part.Function); name != "" && part.ID&15 == 0 && allowed[index] {
				c.Fans[name] = BambuControl{Index: index, Min: int(part.Range & 65535), Max: int(part.Range >> 16), Modern: true}
			}
		}
	} else {
		for index, name := range []string{"part", "auxiliary", "chamber"} {
			if _, reported := b.fans()[name]; !reported {
				continue
			}
			if name == "auxiliary" && b.AuxSupported != nil && !*b.AuxSupported {
				continue
			}
			if name == "chamber" && b.ChamberFanSupported != nil && !*b.ChamberFanSupported {
				continue
			}
			c.Fans[name] = BambuControl{Index: index + 1, Max: 100}
		}
	}
	return c
}

type bambuControlRequest struct {
	Control  string `json:"control"`
	Fan      string `json:"fan,omitempty"`
	Extruder *int   `json:"extruder,omitempty"`
	Value    *int   `json:"value"`
}

func (c *BambuControls) command(in bambuControlRequest) (map[string]any, error) {
	if c == nil || in.Value == nil {
		return nil, errors.New("Please provide a valid printer control and a whole-number value.")
	}
	value := *in.Value
	switch in.Control {
	case "chamber":
		if c.Chamber == nil || value < c.Chamber.Min || value > c.Chamber.Max {
			return nil, errors.New("That chamber temperature is outside your printer's supported range.")
		}
		return map[string]any{"command": "set_ctt", "ctt_val": value}, nil
	case "fan":
		fan, ok := c.Fans[in.Fan]
		if !ok || value < fan.Min || value > fan.Max || value < 0 || value > 100 {
			return nil, errors.New("The fan speed can't be adjusted to that level in the current air-duct mode.")
		}
		if fan.Modern {
			return map[string]any{"command": "set_fan", "fan_index": fan.Index, "speed": value}, nil
		}
		step := (value + 5) / 10
		gear := (3*step + 1) / 2
		return map[string]any{"command": "gcode_line", "param": fmt.Sprintf("M106 P%d S%d\n", fan.Index, gear*17)}, nil
	}
	return nil, errors.New("Unrecognized printer control.")
}

func (s *Server) deviceControl(w http.ResponseWriter, r *http.Request) {
	p := s.sends.printer(r.PathValue("printer"))
	if p == nil || p.Kind == "bambu-connect" {
		fail(w, 404, "printer_doesnt_have_active")
		return
	}
	var in bambuControlRequest
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 1024)).Decode(&in); err != nil {
		fail(w, 400, "expected_printer_control_name")
		return
	}
	status, ok := s.sends.hub.current(p.Printer)
	if !ok || !status.Online {
		fail(w, 409, "hang_second_waiting_live")
		return
	}
	command, err := status.Controls.command(in)
	scope := "print"
	if in.Value != nil {
		value := *in.Value
		switch in.Control {
		case "nozzle", "bed":
			maxTemp, gcode := 120, "M140"
			if in.Control == "nozzle" {
				maxTemp, gcode = 300, "M104"
			}
			if in.Control == "nozzle" && len(status.NozzleTemps) > 0 {
				err = errors.New("Pick an available extruder and a temperature between 0 and 350 °C.")
				for _, extruder := range status.NozzleTemps {
					if in.Extruder != nil && extruder.ID == *in.Extruder && value >= 0 && value <= 350 {
						command, err = map[string]any{"command": "set_nozzle_temp", "extruder_index": extruder.ID, "target_temp": value}, nil
					}
				}
			} else if value >= 0 && value <= maxTemp {
				command, err = map[string]any{"command": "gcode_line", "param": fmt.Sprintf("%s S%d\n", gcode, value)}, nil
			} else {
				err = errors.New("That target temperature is outside your printer's supported range.")
			}
		case "speed":
			if value >= 1 && value <= 4 {
				command, err = map[string]any{"command": "print_speed", "param": fmt.Sprint(value)}, nil
			} else {
				err = errors.New("Speed mode must be between 1 (Silent) and 4 (Ludicrous).")
			}
		case "light":
			if value == 0 || value == 1 {
				scope = "system"
				command, err = map[string]any{"command": "ledctrl", "led_node": "chamber_light", "led_mode": map[bool]string{true: "on", false: "off"}[value == 1], "led_on_time": 500, "led_off_time": 500, "loop_times": 0, "interval_time": 0}, nil
			} else {
				err = errors.New("Light setting must be 0 (off) or 1 (on).")
			}
		}
	}
	if err != nil {
		fail(w, 422, "bambu_controls_unprocessable", err.Error())
		return
	}
	secret, err := p.secret()
	if err != nil {
		fail(w, 409, "bambu_controls_conflict", err.Error())
		return
	}
	if ok, retry := s.sends.allowBurst("device "+s.clientIP(r), deviceBurst, deviceRate); !ok {
		w.Header().Set("Retry-After", fmt.Sprint(min(retry, 86400)))
		fail(w, 429, "slow_down_too_many")
		return
	}
	ctx, cancel := context.WithTimeout(r.Context(), controlWait)
	defer cancel()
	if p.Kind == "bambu-lan" {
		result := s.runBambuCommand(ctx, p, secret, bambuCommand{scope: scope, body: command, codes: "control", what: "the " + in.Control + " change",
			judge: shownBy(in.shownBy)})
		audit(map[string]any{"event": "printer_device_control", "printer": p.Printer, "control": in.Control, "fan": in.Fan, "value": *in.Value, "client": s.clientIP(r), "state": result.State, "error": result.Error})
		writeJSON(w, result.status(), result)
		return
	}
	err = controlHostDevice(ctx, p, secret, in)
	audit(map[string]any{"event": "printer_device_control", "printer": p.Printer, "control": in.Control, "fan": in.Fan, "value": *in.Value, "client": s.clientIP(r), "error": fmt.Sprint(err)})
	if err != nil {
		fail(w, 502, "bambu_controls_upstream", redact(err.Error(), secret))
		return
	}
	s.sends.hub.poke(p.Printer)
	w.WriteHeader(204)
}

const controlWait = 10 * time.Second

func (in bambuControlRequest) shownBy(st PrinterStatus) bool {
	value := float64(*in.Value)
	switch in.Control {
	case "nozzle":
		if len(st.NozzleTemps) > 0 {
			for _, n := range st.NozzleTemps {
				if in.Extruder != nil && n.ID == *in.Extruder {
					return n.Target == value
				}
			}
			return false
		}
		return st.Nozzle != nil && st.Nozzle.Target == value
	case "bed":
		return st.Bed != nil && st.Bed.Target == value
	case "chamber":
		return st.Chamber != nil && st.Chamber.Target == value
	case "fan":
		reported, ok := st.Fans[in.Fan]
		if !ok || st.Controls == nil {
			return false
		}
		if st.Controls.Fans[in.Fan].Modern {
			return reported == value
		}
		return math.Abs(reported-float64((*in.Value+5)/10*10)) <= 5
	case "speed":
		return st.SpeedMode != nil && float64(*st.SpeedMode) == value
	case "light":
		return st.Light != nil && *st.Light == (value == 1)
	}
	return false
}
