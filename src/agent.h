/* RELIC - the debug agent's host boundary.
 *
 * A named-pipe debugger embedded in the PPM host: jump to a part, step
 * it a tick at a time, read a symbol or a word of memory, write a PPM.
 * An MCP server on the other end of the pipe presents it as tools.
 *
 * WIN32 ONLY, both configurations. Everything below collapses to three
 * stubs elsewhere and Agent_Start fails so a host can say so rather
 * than spin - agent_mcp.txt 4.
 *
 * The protocol is docs/subsystems/agent_mcp.txt.
 */

#ifndef RELIC_AGENT_H
#define RELIC_AGENT_H

/* May block inside Agent_Pump while the demo is paused. Only a host
 * with no window may ask for this - a blocked message loop is a
 * window Windows paints as "not responding". */
#define AGENT_PUMP_BLOCK  0x1
/* The agent, not the host, calls Demo_Frame while free-running. */
#define AGENT_PUMP_CLOCK  0x2

/* Load the map, open the pipe, start the listener. 0 = ok. Relative
 * DUMP paths resolve against dump_dir. */
int  Agent_Start(const char *dump_dir);
void Agent_Stop(void);

/* Serve whatever the pipe has parked, on the CALLING thread. Returns
 * non-zero when the client has asked the host to exit. */
int  Agent_Pump(int flags);

#endif /* RELIC_AGENT_H */
