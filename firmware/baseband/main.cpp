/*
 * Copyright (C) 2014 Jared Boone, ShareBrained Technology, Inc.
 *
 * This file is part of PortaPack.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#include "ch.h"
#include "test.h"

#include "lpc43xx_cpp.hpp"

#include "portapack_shared_memory.hpp"
#include "portapack_dma.hpp"

#include "gpdma.hpp"

#include "baseband.hpp"
#include "baseband_dma.hpp"

#include "event_m4.hpp"

#include "irq_ipc_m4.hpp"

#include "rssi.hpp"
#include "rssi_dma.hpp"

#include "touch_dma.hpp"

#include "dsp_decimate.hpp"
#include "dsp_demodulate.hpp"
#include "dsp_fft.hpp"
#include "dsp_fir_taps.hpp"
#include "dsp_iir.hpp"
#include "dsp_iir_config.hpp"
#include "dsp_squelch.hpp"

#include "baseband_stats_collector.hpp"
#include "rssi_stats_collector.hpp"

#include "channel_decimator.hpp"
#include "baseband_processor.hpp"
#include "proc_am_audio.hpp"
#include "proc_nfm_audio.hpp"
#include "proc_wfm_audio.hpp"
#include "proc_ais.hpp"
#include "proc_wideband_spectrum.hpp"
#include "proc_tpms.hpp"

#include "clock_recovery.hpp"
#include "packet_builder.hpp"

#include "message_queue.hpp"

#include "utility.hpp"

#include "debug.hpp"

#include "audio.hpp"
#include "audio_dma.hpp"

#include "gcc.hpp"

#include <cstdint>
#include <cstddef>
#include <array>
#include <string>
#include <bitset>
#include <math.h>

static baseband::Direction direction = baseband::Direction::Receive;

class ThreadBase {
public:
	constexpr ThreadBase(
		const char* const name
	) : name { name }
	{
	}

	static msg_t fn(void* arg) {
		auto obj = static_cast<ThreadBase*>(arg);
		chRegSetThreadName(obj->name);
		obj->run();

		return 0;
	}

	virtual void run() = 0;

private:
	const char* const name;
};

class BasebandThread : public ThreadBase {
public:
	BasebandThread(
	) : ThreadBase { "baseband" }
	{
	}

	Thread* start(const tprio_t priority) {
		return chThdCreateStatic(wa, sizeof(wa),
			priority, ThreadBase::fn,
			this
		);
	}

	Thread* thread_main { nullptr };
	Thread* thread_rssi { nullptr };
	BasebandProcessor* baseband_processor { nullptr };
	BasebandConfiguration baseband_configuration;

private:
	WORKING_AREA(wa, 2048);

	void run() override {
		BasebandStatsCollector stats {
			chSysGetIdleThread(),
			thread_main,
			thread_rssi,
			chThdSelf()
		};

		while(true) {
			if (direction == baseband::Direction::Transmit) {
				const auto buffer_tmp = baseband::dma::wait_for_tx_buffer();
				
				const buffer_c8_t buffer {
					buffer_tmp.p, buffer_tmp.count, baseband_configuration.sampling_rate
				};

				if( baseband_processor ) {
					baseband_processor->execute(buffer);
				}

				stats.process(buffer,
					[](const BasebandStatistics statistics) {
						const BasebandStatisticsMessage message { statistics };
						shared_memory.application_queue.push(message);
					}
				);
			} else {
				const auto buffer_tmp = baseband::dma::wait_for_rx_buffer();
				
				const buffer_c8_t buffer {
					buffer_tmp.p, buffer_tmp.count, baseband_configuration.sampling_rate
				};

				if( baseband_processor ) {
					baseband_processor->execute(buffer);
				}

				stats.process(buffer,
					[](const BasebandStatistics statistics) {
						const BasebandStatisticsMessage message { statistics };
						shared_memory.application_queue.push(message);
					}
				);
			}
		}
	}
};

class RSSIThread : public ThreadBase {
public:
	RSSIThread(
	) : ThreadBase { "rssi" }
	{
	}

	Thread* start(const tprio_t priority) {
		return chThdCreateStatic(wa, sizeof(wa),
			priority, ThreadBase::fn,
			this
		);
	}

	uint32_t sampling_rate { 400000 };

private:
	WORKING_AREA(wa, 128);

	void run() override {
		RSSIStatisticsCollector stats;

		while(true) {
			// TODO: Place correct sampling rate into buffer returned here:
			const auto buffer_tmp = rf::rssi::dma::wait_for_buffer();
			const rf::rssi::buffer_t buffer {
				buffer_tmp.p, buffer_tmp.count, sampling_rate
			};

			stats.process(
				buffer,
				[](const RSSIStatistics statistics) {
					const RSSIStatisticsMessage message { statistics };
					shared_memory.application_queue.push(message);
				}
			);
		}
	}
};


static const int8_t sintab[1024] = {
0, 1, 2, 2, 3, 4, 5, 5, 6, 7, 8, 9, 9, 10, 11, 12, 12, 13, 14, 15, 16, 16, 17, 18, 19, 19, 20, 21, 22, 22, 23, 24, 25, 26, 26, 27, 28, 29, 29, 30, 31, 32, 32, 33, 34, 35, 35, 36, 37, 38, 38, 39, 40, 41, 41, 42, 43, 44, 44, 45, 46, 46, 47, 48, 49, 49, 50, 51, 51, 52, 53, 54, 54, 55, 56, 56, 57, 58, 58, 59, 60, 61, 61, 62, 63, 63, 64, 65, 65, 66, 67, 67, 68, 69, 69, 70, 71, 71, 72, 72, 73, 74, 74, 75, 76, 76, 77, 78, 78, 79, 79, 80, 81, 81, 82, 82, 83, 84, 84, 85, 85, 86, 86, 87, 88, 88, 89, 89, 90, 90, 91, 91, 92, 93, 93, 94, 94, 95, 95, 96, 96, 97, 97, 98, 98, 99, 99, 100, 100, 101, 101, 102, 102, 102, 103, 103, 104, 104, 105, 105, 106, 106, 106, 107, 107, 108, 108, 109, 109, 109, 110, 110, 111, 111, 111, 112, 112, 112, 113, 113, 113, 114, 114, 114, 115, 115, 115, 116, 116, 116, 117, 117, 117, 118, 118, 118, 118, 119, 119, 119, 120, 120, 120, 120, 121, 121, 121, 121, 122, 122, 122, 122, 122, 123, 123, 123, 123, 123, 124, 124, 124, 124, 124, 124, 125, 125, 125, 125, 125, 125, 125, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127,
127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 126, 125, 125, 125, 125, 125, 125, 125, 124, 124, 124, 124, 124, 124, 123, 123, 123, 123, 123, 122, 122, 122, 122, 122, 121, 121, 121, 121, 120, 120, 120, 120, 119, 119, 119, 118, 118, 118, 118, 117, 117, 117, 116, 116, 116, 115, 115, 115, 114, 114, 114, 113, 113, 113, 112, 112, 112, 111, 111, 111, 110, 110, 109, 109, 109, 108, 108, 107, 107, 106, 106, 106, 105, 105, 104, 104, 103, 103, 102, 102, 102, 101, 101, 100, 100, 99, 99, 98, 98, 97, 97, 96, 96, 95, 95, 94, 94, 93, 93, 92, 91, 91, 90, 90, 89, 89, 88, 88, 87, 86, 86, 85, 85, 84, 84, 83, 82, 82, 81, 81, 80, 79, 79, 78, 78, 77, 76, 76, 75, 74, 74, 73, 72, 72, 71, 71, 70, 69, 69, 68, 67, 67, 66, 65, 65, 64, 63, 63, 62, 61, 61, 60, 59, 58, 58, 57, 56, 56, 55, 54, 54, 53, 52, 51, 51, 50, 49, 49, 48, 47, 46, 46, 45, 44, 44, 43, 42, 41, 41, 40, 39, 38, 38, 37, 36, 35, 35, 34, 33, 32, 32, 31, 30, 29, 29, 28, 27, 26, 26, 25, 24, 23, 22, 22, 21, 20, 19, 19, 18, 17, 16, 16, 15, 14, 13, 12, 12, 11, 10, 9, 9, 8, 7, 6, 5, 5, 4, 3, 2, 2, 1, 0, -1, -2, -2, -3, -4, -5, -5, -6, -7, -8, -9, -9, -10, -11, -12, -12, -13, -14, -15, -16, -16, -17, -18, -19, -19, -20, -21, -22, -22, -23, -24, -25, -26, -26, -27, -28, -29, -29, -30, -31,
-32, -32, -33, -34, -35, -35, -36, -37, -38, -38, -39, -40, -41, -41, -42, -43, -44, -44, -45, -46, -46, -47, -48, -49, -49, -50, -51, -51, -52, -53, -54, -54, -55, -56, -56, -57, -58, -58, -59, -60, -61, -61, -62, -63, -63, -64, -65, -65, -66, -67, -67, -68, -69, -69, -70, -71, -71, -72, -72, -73, -74, -74, -75, -76, -76, -77, -78, -78, -79, -79, -80, -81, -81, -82, -82, -83, -84, -84, -85, -85, -86, -86, -87, -88, -88, -89, -89, -90, -90, -91, -91, -92, -93, -93, -94, -94, -95, -95, -96, -96, -97, -97, -98, -98, -99, -99, -100, -100, -101, -101, -102, -102, -102, -103, -103, -104, -104, -105, -105, -106, -106, -106, -107, -107, -108, -108, -109, -109, -109, -110, -110, -111, -111, -111, -112, -112, -112, -113, -113, -113, -114, -114, -114, -115, -115, -115, -116, -116, -116, -117, -117, -117, -118, -118, -118, -118, -119, -119, -119, -120, -120, -120, -120, -121, -121, -121, -121, -122, -122, -122, -122, -122, -123, -123, -123, -123, -123, -124, -124, -124, -124, -124, -124, -125, -125, -125, -125, -125, -125, -125, -126, -126, -126, -126, -126, -126, -126, -126, -126, -126, -126, -127, -127, -127, -127, -127, -127, -127, -127, -127, -127, -127, -127, -127, -127, -127, -127, -127, -127, -127, -127, -127, -127, -127, -127, -127, -127, -127, -127, -127,
-126, -126, -126, -126, -126, -126, -126, -126, -126, -126, -126, -125, -125, -125, -125, -125, -125, -125, -124, -124, -124, -124, -124, -124, -123, -123, -123, -123, -123, -122, -122, -122, -122, -122, -121, -121, -121, -121, -120, -120, -120, -120, -119, -119, -119, -118, -118, -118, -118, -117, -117, -117, -116, -116, -116, -115, -115, -115, -114, -114, -114, -113, -113, -113, -112, -112, -112, -111, -111, -111, -110, -110, -109, -109, -109, -108, -108, -107, -107, -106, -106, -106, -105, -105, -104, -104, -103, -103, -102, -102, -102, -101, -101, -100, -100, -99, -99, -98, -98, -97, -97, -96, -96, -95, -95, -94, -94, -93, -93, -92, -91, -91, -90, -90, -89, -89, -88, -88, -87, -86, -86, -85, -85, -84, -84, -83, -82, -82, -81, -81, -80, -79, -79, -78, -78, -77, -76, -76, -75, -74, -74, -73, -72, -72, -71, -71, -70, -69, -69, -68, -67, -67, -66, -65, -65, -64, -63, -63, -62, -61, -61, -60, -59, -58, -58, -57, -56, -56, -55, -54, -54, -53, -52, -51, -51, -50, -49, -49, -48, -47, -46, -46, -45, -44, -44, -43, -42, -41, -41, -40, -39, -38, -38, -37, -36, -35, -35, -34, -33, -32, -32, -31, -30, -29, -29, -28, -27, -26, -26, -25, -24, -23, -22, -22, -21, -20, -19, -19, -18, -17, -16, -16, -15, -14, -13, -12, -12, -11, -10, -9, -9, -8, -7, -6, -5, -5, -4, -3, -2, -2, -1
};

#define SAMPLES_PER_BIT 192
#define FILTER_SIZE 576
#define SAMPLE_BUFFER_SIZE SAMPLES_PER_BIT + FILTER_SIZE

static int32_t waveform_biphase[] = {
	165,167,168,168,167,166,163,160,
	157,152,147,141,134,126,118,109,
	99,88,77,66,53,41,27,14,
	0,-14,-29,-44,-59,-74,-89,-105,
	-120,-135,-150,-165,-179,-193,-206,-218,
	-231,-242,-252,-262,-271,-279,-286,-291,
	-296,-299,-301,-302,-302,-300,-297,-292,
	-286,-278,-269,-259,-247,-233,-219,-202,
	-185,-166,-145,-124,-101,-77,-52,-26,
	0,27,56,85,114,144,175,205,
	236,266,296,326,356,384,412,439,
	465,490,513,535,555,574,590,604,
	616,626,633,637,639,638,633,626,
	616,602,586,565,542,515,485,451,
	414,373,329,282,232,178,121,62,
	0,-65,-132,-202,-274,-347,-423,-500,
	-578,-656,-736,-815,-894,-973,-1051,-1128,
	-1203,-1276,-1347,-1415,-1479,-1540,-1596,-1648,
	-1695,-1736,-1771,-1799,-1820,-1833,-1838,-1835,
	-1822,-1800,-1767,-1724,-1670,-1605,-1527,-1437,
	-1334,-1217,-1087,-943,-785,-611,-423,-219,
	0,235,487,755,1040,1341,1659,1994,
	2346,2715,3101,3504,3923,4359,4811,5280,
	5764,6264,6780,7310,7856,8415,8987,9573,
	10172,10782,11404,12036,12678,13329,13989,14656,
	15330,16009,16694,17382,18074,18767,19461,20155,
	20848,21539,22226,22909,23586,24256,24918,25571,
	26214,26845,27464,28068,28658,29231,29787,30325,
	30842,31339,31814,32266,32694,33097,33473,33823,
	34144,34437,34699,34931,35131,35299,35434,35535,
	35602,35634,35630,35591,35515,35402,35252,35065,
	34841,34579,34279,33941,33566,33153,32702,32214,
	31689,31128,30530,29897,29228,28525,27788,27017,
	26214,25379,24513,23617,22693,21740,20761,19755,
	18725,17672,16597,15501,14385,13251,12101,10935,
	9755,8563,7360,6148,4927,3701,2470,1235,
	0,-1235,-2470,-3701,-4927,-6148,-7360,-8563,
	-9755,-10935,-12101,-13251,-14385,-15501,-16597,-17672,
	-18725,-19755,-20761,-21740,-22693,-23617,-24513,-25379,
	-26214,-27017,-27788,-28525,-29228,-29897,-30530,-31128,
	-31689,-32214,-32702,-33153,-33566,-33941,-34279,-34579,
	-34841,-35065,-35252,-35402,-35515,-35591,-35630,-35634,
	-35602,-35535,-35434,-35299,-35131,-34931,-34699,-34437,
	-34144,-33823,-33473,-33097,-32694,-32266,-31814,-31339,
	-30842,-30325,-29787,-29231,-28658,-28068,-27464,-26845,
	-26214,-25571,-24918,-24256,-23586,-22909,-22226,-21539,
	-20848,-20155,-19461,-18767,-18074,-17382,-16694,-16009,
	-15330,-14656,-13989,-13329,-12678,-12036,-11404,-10782,
	-10172,-9573,-8987,-8415,-7856,-7310,-6780,-6264,
	-5764,-5280,-4811,-4359,-3923,-3504,-3101,-2715,
	-2346,-1994,-1659,-1341,-1040,-755,-487,-235,
	0,219,423,611,785,943,1087,1217,
	1334,1437,1527,1605,1670,1724,1767,1800,
	1822,1835,1838,1833,1820,1799,1771,1736,
	1695,1648,1596,1540,1479,1415,1347,1276,
	1203,1128,1051,973,894,815,736,656,
	578,500,423,347,274,202,132,65,
	0,-62,-121,-178,-232,-282,-329,-373,
	-414,-451,-485,-515,-542,-565,-586,-602,
	-616,-626,-633,-638,-639,-637,-633,-626,
	-616,-604,-590,-574,-555,-535,-513,-490,
	-465,-439,-412,-384,-356,-326,-296,-266,
	-236,-205,-175,-144,-114,-85,-56,-27,
	0,26,52,77,101,124,145,166,
	185,202,219,233,247,259,269,278,
	286,292,297,300,302,302,301,299,
	296,291,286,279,271,262,252,242,
	231,218,206,193,179,165,150,135,
	120,105,89,74,59,44,29,14,
	0,-14,-27,-41,-53,-66,-77,-88,
	-99,-109,-118,-126,-134,-141,-147,-152,
	-157,-160,-163,-166,-167,-168,-168,-167
};

/*
class RDSProcessor : public BasebandProcessor {
public:
	void execute(buffer_c8_t buffer) override {
        
		for (size_t i = 0; i<buffer.count; i++) {
			
			//Sample generation 2.28M/10=228kHz
			if(s >= 9) {
				s = 0;
				if(sample_count >= SAMPLES_PER_BIT) {
					cur_bit = (shared_memory.rdsdata[(bit_pos / 26) & 15]>>(25-(bit_pos % 26))) & 1;
					prev_output = cur_output;
					cur_output = prev_output ^ cur_bit;

					int32_t *src = waveform_biphase;
					int idx = in_sample_index;

					for(int j=0; j<FILTER_SIZE; j++) {
						val = (*src++);
						if (cur_output) val = -val;
						sample_buffer[idx++] += val;
						if (idx >= SAMPLE_BUFFER_SIZE) idx = 0;
					}

					in_sample_index += SAMPLES_PER_BIT;
					if (in_sample_index >= SAMPLE_BUFFER_SIZE) in_sample_index -= SAMPLE_BUFFER_SIZE;
					
					bit_pos++;
					sample_count = 0;
				}
				
				sample = sample_buffer[out_sample_index];
				sample_buffer[out_sample_index] = 0;
				out_sample_index++;
				if (out_sample_index >= SAMPLE_BUFFER_SIZE) out_sample_index = 0;
				
				//AM @ 228k/4=57kHz
				switch (mphase) {
					case 0:
					case 2: sample = 0; break;
					case 1: break;
					case 3: sample = -sample; break;
				}
				mphase++;
				if (mphase >= 4) mphase = 0;
				
				sample_count++;
			} else {
				s++;
			}
			
			//FM
			frq = (sample>>16) * 386760;
			
			phase = (phase + frq);
			sphase = phase + (256<<16);

			re = sintab[(sphase & 0x03FF0000)>>16];
			im = sintab[(phase & 0x03FF0000)>>16];
			
			buffer.p[i] = {(int8_t)re,(int8_t)im};
		}
	}

private:
	int8_t re, im;
	uint8_t mphase, s;
    uint32_t bit_pos;
    int32_t sample_buffer[SAMPLE_BUFFER_SIZE] = {0};
    int32_t val;
    uint8_t prev_output = 0;
    uint8_t cur_output = 0;
    uint8_t cur_bit = 0;
    int sample_count = SAMPLES_PER_BIT;
    int in_sample_index = 0;
    int32_t sample;
    int out_sample_index = SAMPLE_BUFFER_SIZE-1;
	uint32_t phase, sphase;
	int32_t sig, frq, frq_im, rdsc;
	int32_t k;
};*/

class LCRFSKProcessor : public BasebandProcessor {
public:
	void execute(buffer_c8_t buffer) override {
        
		for (size_t i = 0; i<buffer.count; i++) {
			
			//Sample generation 2.28M/10 = 228kHz
			if (s >= 9) {
				s = 0;
				
				if (sample_count >= shared_memory.afsk_samples_per_bit) {
					if (shared_memory.afsk_transmit_done == false)
						cur_byte = shared_memory.lcrdata[byte_pos];
					if (!cur_byte) {
						if (shared_memory.afsk_repeat) {
							shared_memory.afsk_repeat--;
							bit_pos = 0;
							byte_pos = 0;
							cur_byte = shared_memory.lcrdata[0];
							message.n = shared_memory.afsk_repeat;
							shared_memory.application_queue.push(message);
						} else {
							message.n = 0;
							shared_memory.afsk_transmit_done = true;
							shared_memory.application_queue.push(message);
							cur_byte = 0;
						}
					}
					
					gbyte = 0;
					gbyte = cur_byte << 1;
					gbyte |= 1;
					
					cur_bit = (gbyte >> (9-bit_pos)) & 1;

					if (bit_pos == 9) {
						bit_pos = 0;
						byte_pos++;
					} else {
						bit_pos++;
					}
					
					//aphase = 0x2FFFFFF;
					
					sample_count = 0;
				} else {
					sample_count++;
				}
				if (cur_bit)
					aphase += shared_memory.afsk_phase_inc_mark; //(353205)
				else
					aphase += shared_memory.afsk_phase_inc_space; //(647542)
					
				sample = sintab[(aphase & 0x03FF0000)>>16];
			} else {
				s++;
			}
			
			sample = sintab[(aphase & 0x03FF0000)>>16];
			
			//FM
			frq = sample * shared_memory.afsk_fmmod;
			
			phase = (phase + frq);
			sphase = phase + (256<<16);

			re = sintab[(sphase & 0x03FF0000)>>16];
			im = sintab[(phase & 0x03FF0000)>>16];
			
			buffer.p[i] = {(int8_t)re,(int8_t)im};
		}
	}

private:
	int8_t re, im;
	uint8_t s;
    uint8_t bit_pos, byte_pos = 0;
    char cur_byte = 0;
    uint16_t gbyte;
    uint8_t cur_bit = 0;
    uint32_t sample_count;
	uint32_t aphase, phase, sphase;
	int32_t sample, sig, frq;
	TXDoneMessage message;
};

/*class ToneProcessor : public BasebandProcessor {
public:
	void execute(buffer_c8_t buffer) override {
        
		for (size_t i = 0; i<buffer.count; i++) {
			
			//Sample generation 2.28M/10 = 228kHz
			if (s >= 9) {
				s = 0;
				aphase += 353205;	// DEBUG
				sample = sintab[(aphase & 0x03FF0000)>>16];
			} else {
				s++;
			}
			
			sample = sintab[(aphase & 0x03FF0000)>>16];
			
			//FM
			frq = sample * 500;		// DEBUG
			
			phase = (phase + frq);
			sphase = phase + (256<<16);

			re = sintab[(sphase & 0x03FF0000)>>16];
			im = sintab[(phase & 0x03FF0000)>>16];
			
			buffer.p[i] = {(int8_t)re,(int8_t)im};
		}
	}

private:
	int8_t re, im;
	uint8_t s;
    uint32_t sample_count;
	uint32_t aphase, phase, sphase;
	int32_t sample, sig, frq;
};*/


#define POLY_MASK_32 0xB4BCD35C

class JammerProcessor : public BasebandProcessor {
public:
	void execute(buffer_c8_t buffer) override {
        
		for (size_t i = 0; i<buffer.count; i++) {

			/*if (s > 3000000) {
				s = 0;
				feedback = lfsr & 1;
				lfsr >>= 1;
				if (feedback == 1)
					lfsr ^= POLY_MASK_32;
			} else {
				s++;
			}

			aphase += lfsr;*/
			
			/*if (s >= 10) {
				s = 0;
				aphase += 353205;	// DEBUG
			} else {
				s++;
			}
			
			sample = sintab[(aphase & 0x03FF0000)>>16];*/
			
			// Duration timer
			// 
			if (s >= 10000) { //shared_memory.jammer_ranges[ir].duration
				s = 0;
				for (;;) {
					ir++;
					if (ir > 15) ir = 0;
					if (shared_memory.jammer_ranges[ir].active == true) break;
				}
				jammer_bw = shared_memory.jammer_ranges[ir].width;
				
				message.freq = shared_memory.jammer_ranges[ir].center;
				shared_memory.application_queue.push(message);
			} else {
				s++;
			}
			
			// Ramp
			/*if (r >= 10) {
				if (sample < 128)
					sample++;
				else
					sample = -127;
				r = 0;
			} else {
				r++;
			}*/
			
			// Phase
			if (r >= 70) {
				aphase += ((aphase>>4) ^ 0x4573) << 14;
				r = 0;
			} else {
				r++;
			}
			
			aphase += 35320;
			sample = sintab[(aphase & 0x03FF0000)>>16];
			
			//FM
			frq = sample * jammer_bw;		// Bandwidth

			//65536 -> 0.6M
			//131072 -> 1.2M
			
			phase = (phase + frq);
			sphase = phase + (256<<16);

			re = sintab[(sphase & 0x03FF0000)>>16];
			im = sintab[(phase & 0x03FF0000)>>16];
			
			buffer.p[i] = {(int8_t)re,(int8_t)im};
		}
	}

private:
    int32_t lfsr32 = 0xABCDE;
    uint32_t s;
	int8_t r, ir, re, im;
	int64_t jammer_bw, jammer_center;
	int feedback;
	int32_t lfsr;
    uint32_t sample_count;
	uint32_t aphase, phase, sphase;
	int32_t sample, frq;
	RetuneMessage message;
};

extern "C" {

void __late_init(void) {
	/*
	 * System initializations.
	 * - HAL initialization, this also initializes the configured device drivers
	 *   and performs the board-specific initializations.
	 * - Kernel initialization, the main() function becomes a thread and the
	 *   RTOS is active.
	 */
	halInit();

	/* After this call, scheduler, systick, heap, etc. are available. */
	/* By doing chSysInit() here, it runs before C++ constructors, which may
	 * require the heap.
	 */
	chSysInit();
}

}

static BasebandThread baseband_thread;
static RSSIThread rssi_thread;

static void init() {
	i2s::i2s0::configure(
		audio::i2s0_config_tx,
		audio::i2s0_config_rx,
		audio::i2s0_config_dma
	);

	audio::dma::init();
	audio::dma::configure();
	audio::dma::enable();

	i2s::i2s0::tx_start();
	i2s::i2s0::rx_start();

	LPC_CREG->DMAMUX = portapack::gpdma_mux;
	gpdma::controller.enable();
	nvicEnableVector(DMA_IRQn, CORTEX_PRIORITY_MASK(LPC_DMA_IRQ_PRIORITY));

	baseband::dma::init();

	rf::rssi::init();
	touch::dma::init();

	const auto thread_main = chThdSelf();
	
	const auto thread_rssi = rssi_thread.start(NORMALPRIO + 10);

	baseband_thread.thread_main = thread_main;
	baseband_thread.thread_rssi = thread_rssi;

	baseband_thread.start(NORMALPRIO + 20);
}

static void shutdown() {
	// TODO: Is this complete?
	
	nvicDisableVector(DMA_IRQn);

	m0apptxevent_interrupt_disable();
	
	chSysDisable();

	systick_stop();
}

static void halt() {
	port_disable();
	while(true) {
		port_wait_for_interrupt();
	}
}

class EventDispatcher {
public:
	MessageHandlerMap& message_handlers() {
		return message_map;
	}

	void run() {
		while(is_running) {
			const auto events = wait();
			dispatch(events);
		}
	}

	void request_stop() {
		is_running = false;
	}

private:
	MessageHandlerMap message_map;

	bool is_running = true;

	eventmask_t wait() {
		return chEvtWaitAny(ALL_EVENTS);
	}

	void dispatch(const eventmask_t events) {
		if( events & EVT_MASK_BASEBAND ) {
			handle_baseband_queue();
		}

		if( events & EVT_MASK_SPECTRUM ) {
			handle_spectrum();
		}
	}

	void handle_baseband_queue() {
		std::array<uint8_t, Message::MAX_SIZE> message_buffer;
		while(Message* const message = shared_memory.baseband_queue.pop(message_buffer)) {
			message_map.send(message);
		}
	}

	void handle_spectrum() {
		if( baseband_thread.baseband_processor ) {
			baseband_thread.baseband_processor->update_spectrum();
		}
	}
};

const auto baseband_buffer =
	new std::array<baseband::sample_t, 8192>();
		
int main(void) {
	init();

	events_initialize(chThdSelf());
	m0apptxevent_interrupt_enable();

	EventDispatcher event_dispatcher;
	auto& message_handlers = event_dispatcher.message_handlers();

	message_handlers.register_handler(Message::ID::BasebandConfiguration,
		[&message_handlers](const Message* const p) {
			auto message = reinterpret_cast<const BasebandConfigurationMessage*>(p);
			if( message->configuration.mode != baseband_thread.baseband_configuration.mode ) {

				if( baseband_thread.baseband_processor ) {
					i2s::i2s0::tx_mute();
					baseband::dma::disable();
					rf::rssi::stop();
				}

				// TODO: Timing problem around disabling DMA and nulling and deleting old processor
				auto old_p = baseband_thread.baseband_processor;
				baseband_thread.baseband_processor = nullptr;
				delete old_p;

				switch(message->configuration.mode) {
				case 0:
					direction = baseband::Direction::Receive;
					baseband_thread.baseband_processor = new NarrowbandAMAudio();
					break;

				case 1:
					direction = baseband::Direction::Receive;
					baseband_thread.baseband_processor = new NarrowbandFMAudio();
					break;

				case 2:
					baseband_thread.baseband_processor = new WidebandFMAudio();
					break;

				case 3:
					direction = baseband::Direction::Receive;
					baseband_thread.baseband_processor = new AISProcessor();
					break;

				case 4:
					direction = baseband::Direction::Receive;
					baseband_thread.baseband_processor = new WidebandSpectrum();
					break;

				case 5:
					direction = baseband::Direction::Receive;
					baseband_thread.baseband_processor = new TPMSProcessor();
					break;

				/*case 15:
					direction = baseband::Direction::Transmit;
					baseband_thread.baseband_processor = new RDSProcessor();
					break;*/
				
				case 16:
					direction = baseband::Direction::Transmit;
					baseband_thread.baseband_processor = new LCRFSKProcessor();
					break;
			
				/*case 17:
					direction = baseband::Direction::Transmit;
					baseband_thread.baseband_processor = new ToneProcessor();
					break;*/
				
				case 18:
					direction = baseband::Direction::Transmit;
					baseband_thread.baseband_processor = new JammerProcessor();
					break;

				default:
					break;
				}

				if( baseband_thread.baseband_processor ) {
					if( direction == baseband::Direction::Receive ) {
						rf::rssi::start();
					}
					baseband::dma::enable(direction);
					rf::rssi::stop();
				}
			}
			
			baseband::dma::configure(
				baseband_buffer->data(),
				direction
			);

			baseband_thread.baseband_configuration = message->configuration;
		}
	);

	message_handlers.register_handler(Message::ID::Shutdown,
		[&event_dispatcher](const Message* const) {
			event_dispatcher.request_stop();
		}
	);

	/* TODO: Ensure DMAs are configured to point at first LLI in chain. */

	rf::rssi::dma::allocate(4, 400);

	touch::dma::allocate();
	touch::dma::enable();
	
	baseband::dma::configure(
		baseband_buffer->data(),
		direction
	);

	//baseband::dma::allocate(4, 2048);

	event_dispatcher.run();

	shutdown();

	ShutdownMessage shutdown_message;
	shared_memory.application_queue.push(shutdown_message);

	halt();

	return 0;
}
