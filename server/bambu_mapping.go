package main

type bambuTrayAddress struct {
	AMS  int `json:"ams_id"`
	Slot int `json:"slot_id"`
}

func isExternalTray(id int) bool { return id == 254 || id == 255 }
