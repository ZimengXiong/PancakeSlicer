package main

import (
	"encoding/json"
	"fmt"
)

func validatePlatePlacement(raw []byte, saving bool) error {
	type placement struct {
		Printable *bool `json:"printable"`
		Plate     int   `json:"plate"`
	}
	var plate struct {
		Objects []struct {
			placement
			Instances []placement `json:"instances"`
		} `json:"objects"`
		Plates []struct {
			Index int `json:"index"`
		} `json:"plates"`
	}
	if err := json.Unmarshal(raw, &plate); err != nil {
		return err
	}
	plates := 1
	for _, p := range plate.Plates {
		plates = max(plates, p.Index)
	}
	for _, object := range plate.Objects {
		plates = max(plates, object.Plate)
		for _, p := range object.Instances {
			plates = max(plates, p.Plate)
		}
	}
	if plates > 1 && !saving {
		return fmt.Errorf("Please slice one plate at a time and send only that plate's objects.")
	}
	live := 0
	for _, object := range plate.Objects {
		places := object.Instances
		if places == nil {
			places = []placement{object.placement}
		}
		if len(places) == 0 {
			return fmt.Errorf("instances must be a non-empty array")
		}
		for _, p := range places {
			printable := p.Printable == nil || *p.Printable
			if printable {
				live++
			}
		}
	}
	if !saving && live == 0 {
		return fmt.Errorf("There are no printable objects on the build plate.")
	}
	return nil
}
