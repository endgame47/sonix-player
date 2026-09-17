# TO-DO for R1 compatibility

## Sonix Player

- [x] Add BOARD parameter to Makefile to support multiple targets  
- [x] Set screen resolution according toe BOARD define  
- [x] Adjust button keymap (no 'previous' button)  
- [ ] LED logic (different controller, fewer available colors)  
- [ ] Charger limiting logic (different controller, no mp2731 device)  
- [ ] Check charge voltage (axp2101)  
- [ ] Hard-coded settings/strings (like About screen)  
- [ ] DAC-specific settings (NOS, DRE, High gain, etc.)  
- [ ] Create patch for cst8xx (similar to R3ProII's gt9xx patch) touchscreen multi-touch  
- [ ] Adapt Gearboy to R1's screen resolution  

## Sonix Packer

- [x] Create copy to build an R1 firmware (exclude R3ProII-specific 'module_driver' assets for now)  
- [ ] Create unified packer that supports both (or more) devices  