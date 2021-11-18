#ifndef __INPUTFILTER_H__
#define __INPUTFILTER_H__

#define IF_EVENT_SQUEEZE_VIB_START 1
#define IF_EVENT_SQUEEZE_VIB_END 2

// report squeeze related event
extern void if_report_squeeze_event(unsigned long timestamp, bool vibration, int num_param);

// report squeeze related wakelock events
extern void if_report_squeeze_wake_event(int nanohub_flag, int vibrator_flag, unsigned long timestamp, int init_event_flag);

#endif /* __INPUTFILTER_H__ */
