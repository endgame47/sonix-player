# The stock script with the ladder read every 30 ms instead of every 100:
# a quick press, or the second press of a double click, falls between two
# readings 100 ms apart and is never seen.
insmod keyboard_adc_multifunc.ko \
	adc_init_value=3300 \
	adc_channel=0 \
	adc_deviation=100 \
	adc_key_detectime=30 \
	debug=0 \
	key1_code=164 key1_reuse_code=165 key1_multifunc_enable=0 key1_value=0 \
	key2_code=114 key2_reuse_code=115 key2_multifunc_enable=0 key2_value=1000 \
	key3_code=115 key3_reuse_code=114 key3_multifunc_enable=0 key3_value=1650 \
	key4_code=-1 key4_reuse_code=-1 key4_multifunc_enable=-1 key4_value=-1 \
	key5_code=-1 key5_reuse_code=-1 key5_multifunc_enable=-1 key5_value=-1 \
	key6_code=-1 key6_reuse_code=-1 key6_multifunc_enable=-1 key6_value=-1 \
	key7_code=-1 key7_reuse_code=-1 key7_multifunc_enable=-1 key7_value=-1 \
	key8_code=-1 key8_reuse_code=-1 key8_multifunc_enable=-1 key8_value=-1 \
