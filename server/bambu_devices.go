package main

import (
	"bytes"
	"encoding/json"
	"math"
	"strconv"
	"strings"
)

type NozzleStatus struct {
	ID       int     `json:"id"`
	Extruder int     `json:"extruder"`
	Diameter float64 `json:"diameter"`
	Type     string  `json:"type,omitempty"`
	Flow     string  `json:"flow,omitempty"`
	Rack     bool    `json:"rack,omitempty"`
	Abnormal bool    `json:"abnormal,omitempty"`
	Legacy   bool    `json:"legacy,omitempty"`
}

type NozzleTemp struct {
	ID     int     `json:"id"`
	Actual float64 `json:"actual"`
	Target float64 `json:"target"`
}

type bambuDevice struct {
	Extruder *struct {
		Info bambuList[struct {
			ID   int     `json:"id"`
			Temp uint32  `json:"temp"`
			Snow *uint32 `json:"snow"`
		}] `json:"info"`
	} `json:"extruder"`
	Airduct *struct {
		ModeCur  int `json:"modeCur"`
		ModeList []struct {
			ID      int   `json:"modeId"`
			Control []int `json:"ctrl"`
		} `json:"modeList"`
		Parts []struct {
			ID       int    `json:"id"`
			Function int    `json:"func"`
			State    int    `json:"state"`
			Range    uint32 `json:"range"`
		} `json:"parts"`
	} `json:"airduct"`
	Nozzle *struct {
		Exist int `json:"exist"`
		Info  bambuList[struct {
			ID       int             `json:"id"`
			Diameter json.RawMessage `json:"diameter"`
			Type     string          `json:"type"`
			Stat     *int            `json:"stat"`
		}] `json:"info"`
	} `json:"nozzle"`
}

type bambuList[T any] []T

func (l *bambuList[T]) UnmarshalJSON(raw []byte) error {
	if text := bytes.TrimSpace(raw); len(text) == 0 || text[0] != '[' {
		*l = nil
		return nil
	}
	return json.Unmarshal(raw, (*[]T)(l))
}

func bambuNumber(raw json.RawMessage) *float64 {
	if len(raw) == 0 || string(raw) == "null" {
		return nil
	}
	text := strings.Trim(string(raw), "\"")
	v, err := strconv.ParseFloat(text, 64)
	if err != nil || math.IsNaN(v) || math.IsInf(v, 0) {
		return nil
	}
	return &v
}

func bambuNozzleType(code string) (string, string) {
	switch code {
	case "hardened_steel", "stainless_steel", "tungsten_carbide", "brass":
		return code, "Standard"
	case "E3D":
		return code, "E3D High Flow"
	}
	if len(code) < 4 {
		return "", ""
	}
	material := map[string]string{"00": "stainless_steel", "01": "hardened_steel", "05": "tungsten_carbide"}[code[2:4]]
	flow := map[byte]string{'S': "Standard", 'A': "Standard", 'X': "Standard", 'H': "High Flow", 'E': "High Flow", 'U': "TPU High Flow", 'B': "E3D High Flow"}[code[1]]
	return material, flow
}

func (b *bambuReport) nozzles() []NozzleStatus {
	if b.Device != nil && b.Device.Nozzle != nil && len(b.Device.Nozzle.Info) > 0 {
		system := b.Device.Nozzle
		dual := system.Exist&2 != 0
		for _, row := range system.Info {
			if row.ID&0xf == 1 && row.ID&0xf0 == 0 {
				dual = true
			}
		}
		out := []NozzleStatus{}
		for _, row := range system.Info {
			diameter := bambuNumber(row.Diameter)
			if diameter == nil || *diameter <= 0 {
				continue
			}
			rack := (row.ID>>4)&0xf == 1
			extruder := 0
			if dual && (rack || row.ID&0xf == 0) {
				extruder = 1
			}
			material, flow := bambuNozzleType(row.Type)
			abnormal := row.Stat != nil && ((*row.Stat>>1)&3) != 0
			out = append(out, NozzleStatus{ID: row.ID, Extruder: extruder, Diameter: *diameter, Type: material, Flow: flow, Rack: rack, Abnormal: abnormal})
		}
		return out
	}
	if diameter := bambuNumber(b.NozzleDiameter); diameter != nil && *diameter > 0 {
		material, flow := bambuNozzleType(b.NozzleType)
		if b.Flag3 != nil {
			flow = "Standard"
			if (*b.Flag3>>10)&7 == 1 {
				flow = "High Flow"
			}
		}
		return []NozzleStatus{{Diameter: *diameter, Type: material, Flow: flow, Legacy: true}}
	}
	return nil
}

func (b *bambuReport) fans() map[string]float64 {
	out := map[string]float64{}
	if b.Device != nil && b.Device.Airduct != nil {
		for _, part := range b.Device.Airduct.Parts {
			if part.ID&15 == 0 {
				if name := bambuFanName(part.Function); name != "" {
					out[name] = float64(part.State & 255)
				}
			}
		}
		return out
	}
	if b.FanGear != nil {
		for i, name := range []string{"part", "auxiliary", "chamber"} {
			out[name] = math.Round(float64((*b.FanGear>>(8*i))&255) * 100 / 255)
		}
	} else {
		for name, raw := range map[string]json.RawMessage{"part": b.PartFan, "auxiliary": b.AuxFan, "chamber": b.ChamberFan} {
			if speed := bambuNumber(raw); speed != nil {
				out[name] = math.Max(0, math.Min(100, math.Floor(*speed/1.5)*10))
			}
		}
	}
	if len(out) == 0 {
		return nil
	}
	return out
}

func (b *bambuReport) activeTray(ams, slot int, external bool) bool {
	if b.Device != nil && b.Device.Extruder != nil && len(b.Device.Extruder.Info) > 0 {
		for _, extruder := range b.Device.Extruder.Info {
			if extruder.Snow != nil {
				value := *extruder.Snow
				unit, tray := int(value>>8&255), int(value&255)
				if external && unit == 255 && tray != 255 || !external && unit == ams && tray == slot {
					return true
				}
			}
		}
		return false
	}
	if b.AMS == nil {
		return false
	}
	now := bambuNumber(b.AMS.TrayNow)
	if now == nil {
		return false
	}
	if external {
		return int(*now) == 254
	}
	return int(*now) == ams*4+slot
}
