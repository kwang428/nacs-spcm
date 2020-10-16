// Written by Kenneth Wang, Oct 2020

#ifndef _NACS_SPCM_STREAM_H
#define _NACS_SPCM_STREAM_H

#include <nacs-utils/thread.h>

#include <complex>
#include <atomic>
#include <thread>
#include <cstring>
#include <ostream>
#include <vector>
#include <condition_variable>

#include <chrono>


using namespace NaCs;

namespace Spcm {

enum class CmdType : uint8_t
{
    // CmdType is a enumerated class that holds all possible commands.
    // They are represented by a uint8_t
    Meta, // Meta command types are in CmdMeta
    AmpSet,
    AmpFn,
    AmpVecFn,
    FreqSet,
    FreqFn,
    FreqVecFn,
    ModChn, // add or delete channels
    Phase,
    _MAX = Phase // keeps track of how many CmdType options there are
}

enum class CmdMeta : uint32_t
{
    Reset,
    ResetAll,
    TriggerEnd,
    TriggerStart
}

// Note freq, amp, and phase are already integers in the command struct
// amp is normalized to (2^(31) -1) * pi = 6.7465185e9f
// freq is 10 times the actual frequency
// phase_scale is 2 / (625e6 * 10). We take the integer phase and multiply it by
// phase_scale to get the actual phase in units of pi. 625e6 * 10 is the max possible frequency.

struct Cmd
{
private:
    static constexpr int op_bits = 4; // number of bits needed to describe the operation
    static constexpr int chn_bits = 32 - op_bits; // number of bits to determine the chn number
    static_assert((int)CmdType::MAX < (1 << op_bits), ""); // ensure op_bits are enough to describe the number of commands. << is the left shift operator.
public:
    static constexpr uint32_t add_chn = (uint32_t(1) << chn_bits) - 1; // code for adding a channel
    uint32_t t; // start time for command 
    uint8_t _op:op_bits; // op should only contain op_bits amount of information.
    uint32_t chn:chn_bits;
    int32_t final_val; // final value at end of command.
    float len = 0; // length of pulse
    void(*fnptr)(void) = nullptr; // function pointer
    CmdType op() const
    {
        return (CmdType)_op; // returns integer index of operation
    }
    // Functions below are used to get the command object for the desired operation
    static Cmd getReset(uint32_t t = 0)
    {
        return Cmd{t, (uint8_t)CmdType::Meta, (uint32_t)CmdMeta::Reset, 0}; //initializer list notation, initializes in the order of declared variables above.
    }
    static Cmd getResetAll(uint32_t t = 0)
    {
        return Cmd{t, (uint8_t)CmdType::Meta, (uint32_t)CmdMeta::ResetAll, 0};
    }
    static Cmd getTriggerEnd(uint32_t t = 0)
    {
        return Cmd{t, (uint8_t)CmdType::Meta, (uint32_t)CmdMeta::TriggerEnd, 0};
    }
    static Cmd getTriggerStart(uint32_t t = 0)
    {
        return Cmd{t, (uint8_t)CmdType::Meta, (uint32_t)CmdMeta::TriggerStart, 0};
    }
    static Cmd getAmpSet(uint32_t t, uint32_t chn, int32_t amp)
    {
        return Cmd{t, (uint8_t)CmdType::AmpSet, chn, amp};
    }
    static Cmd getFreqSet(uint32_t t, uint32_t chn, int32_t freq)
    {
        return Cmd{t, (uint8_t)CmdType::FreqSet, chn, freq};
    }
    static Cmd getPhase(uint32_t t, uint32_t chn, int32_t phase)
    {
        return Cmd{t, (uint8_t)CmdType::Phase, chn, phase};
    }
    static Cmd getAddChn(uint32_t t)
    {
        return Cmd{t, (uint8_t)CmdType::ModChn, add_chn, 0}; // largest possible chn_number interpretted as adding a channel
    }
    static Cmd getDelChn(uint32_t t, uint32_t chn)
    {
        return Cmd{t, (uint8_t)CmdType::ModChn, chn, 0};
    }
    static Cmd getAmpFn(uint32_t t, uint32_t chn, int32_t final_val, float len, void(*fnptr)(void))
    {
        return Cmd{t, (uint8_t)CmdType::AmpFn, chn, final_val, len, fnptr};
    }
    static Cmd getFreqFn(uint32_t t, uint32_t chn, int32_t final_val, float len, void(*fnptr)(void))
    {
        return Cmd{t, (uint8_t)CmdType::FreqFn, chn, final_val, len, fnptr};
    }
    static Cmd getAmpVecFn(uint32_t t, uint32_t chn, int32_t final_val, float len, void(*fnptr)(void))
    {
        return Cmd{t, (uint8_t)CmdType::AmpFn, chn, final_val, len, fnptr};
    }
    static Cmd getFreqVecFn(uint32_t t, uint32_t chn, int32_t final_val, float len, void(*fnptr)(void))
    {
        return Cmd{t, (uint8_t)CmdType::FreqFn, chn, final_val, len, fnptr};
    }
    const char *name() const; // returns name of cmd
    void dump() const;
    inline bool operator==(const Cmd &other) const
    {
        //Checks whether commands are the same or not.
        if (other.t ! = t)
            return false;
        if (other.op() != op())
            return false;
        switch(op())
        {
        case CmdType::AmpSet:
        case CmdType::FreqSet:
        case CmdType::Phase:
        case CmdType::ModChn:
            if (other.final_val != final_val)
                return false;
            return other.chn == chn;
        case CmdType::Meta:
            if (chn == (uint32_t)CmdMeta::TriggerEnd || chn == (uint32_t)CmdMeta::TriggerStart)
                return other.chn == chn && final_val == other.final_val;
        case CmdType::AmpFn:
        case CmdType::FreqFn:
        case CmdType::AmpVecFn:
        case CmdType::FreqVecFn:
            if ((other.final_val == final_val) && (other.len == len))
                return other.fnptr == fnptr;
        default:
            return false;
        }
    }
};

static_assert(sizeof(Cmd) == 20, "");

std::ostream &operator<<(std::ostream &stm, const Cmd &cmd);
std::ostream &operator<<(std::ostream &stm, const std::vector<Cmd> &cmds); //printing functions

struct activeCmd {
// structure to keep track of commands that span longer times
    Cmd* m_cmd;
    std::vector<int32_t> vals; // precalculated values
    activeCmd(Cmd* cmd) : m_cmd(cmd) {
        if (cmd->op() == CmdType::AmpVecFn || cmd->op() == CmdType::FreqVecFn) {
// only precalculate and store if it's vector input. If not calculate in real time.
            std::vector<uint32_t> ts;
            ts.reserve((static_cast<size_t> cmd->len) + 1); // this should truncate.
            for (uint32_t i = 0; i < (cmd->len + 1); i++)
                ts.push_back(i);
            vals = ((std::vector<int32_t>(*)(std::vector<uint32_t>))(cmd->fnptr))(ts);
        }
    }
    std::pair<int32_t,int32_t> eval(uint32_t t);
};

class StreamBase
{
public:
    inline const int16_t *get_output(size_t &sz)
    {
        return m_output.get_read_ptr(sz); // call to obtain values for output 
    }
    inline void consume_output(size_t sz)
    {
        return m_output.read_size(sz); // call after finishing using values for output
    }
    //similar commands for the command pipe to come
    inline size_t copy_cmds(const Cmd *cmds, size_t sz)
    {
        if (!probe_cmd_input()) // return 0 if no commands to consume
            return 0;
        sz = std::min(sz, assume(m_cmd_max_write - m_cmd_wrote));
        std::memcpy(&m_cmd_write_ptr[m_cmd_wrote], cmds, sz * sizeof(Cmd));
        m_cmd_wrote += sz;
        if (m_cmd_wrote == m_cmd_max_write) {
            m_commands.wrote_size(m_cmd_max_write);
            m_cmd_wrote = m_cmd_max_write = 0;
        }
        return sz;
    }
    inline bool try_add_cmd(const Cmd &cmd)
    {
        // adds a single command. returns true if successfully added
        return copy_cmds(&cmd, 1) != 0;
    }
    inline void add_cmd(const Cmd &cmd)
    {
        // keeps on trying to add command until successfully added
        while(!try_add_cmd(cmd)){
            CPU::pause();
        }
    }
    inline void flush_cmd()
    {
        // tells command pipe data has been read.
        // if(uint32_t) returns true if uint32_t is nonzero
        if (m_cmd_wrote) {
            m_cmd_max_write -= m_cmd_wrote;
            m_cmd_write_ptr += m_cmd_wrote; // advances pointer
            m_commands.wrote_size(m_cmd_wrote);
            m_cmd_wrote = 0;
        }
    }
    // RELATED TO TRIGGER. MIGHT NOT BE NEEDED
    inline uint32_t get_end_id()
    {
        return ++m_end_trigger_cnt;
    }
    inline uint32_t get_start_id()
    {
        return ++m_start_trigger_cnt;
    }
    inline bool slow_mode() const
    {
        return m_slow_mode.load(std::memory_order_relaxed);
    }
    uint32_t end_triggered() const
    {
        return m_end_triggered.load(std::memory_order_relaxed);
    }

    void set_time_offset(int64_t offset)
    {
        m_time_offset.store(offset, std::memory_order_relaxed);
    }
    void set_start_trigger(uint32_t v, uint64_t t)
    {
        m_start_trigger_time.store(t, std::memory_order_relaxed);
        m_start_trigger.store(v, std::memory_order_release);
    }
    void set_end_trigger(int16_t *p)
    {
        m_end_trigger.store(p, std::memory_order_relaxed);
    }
    int16_t *end_trigger() const
    {
        return m_end_trigger.load(std::memory_order_relaxed);
    }

protected:
    struct State {
        // structure which keeps track of the state of a channel
        int64_t phase;
        int32_t freq;
        int32_t amp;
    };
    void generate_page(State * states); //workhorse, takes a vector of states for the channels
    StreamBase(double step_t, std::atomic<uint64_t> &cmd_underflow, std::atomic<uint64_t> &underflow) :
        m_step_t(step_t),
        m_cmd_underflow(cmd_underflow),
        m_underflow(underflow)
    {
    }
private:
    inline bool probe_cmd_input()
    {
        // returns true if there are still commands to read, and determines number of cmds ready
        // returns false if no commands left
        if (m_cmd_wrote == m_cmd_max_write){
            m_cmd_wrote = 0;
            m_cmd_write_ptr = m_commands.get_write_ptr(m_cmd_max_write);
            if (!m_cmd_max_write) {
                return false; // return false if no commands
            }
        }
        return true;
    }
    const Cmd *get_cmd_curt();
    const Cmd *get_cmd();
    void cmd_next();
    void step(int16_t *out, State *states); // workhorse function to step to next time
    const Cmd *consume_old_cmds(State * states);
    bool check_start(uint32_t t, uint32_t id);
    void clear_underflow();

    constexpr static uint32_t output_block_sz = 512; // COME BACK TO THIS, WHEN THE UNITS ARE KNOWN
    // Members accessed by worker threads
protected:
    std::atomic_bool m_stop{false};
private:
    std::atomic_bool m_slow_mode{true}; // related to trigger
    uint32_t m_end_trigger_pending{0};
    uint32_t m_end_trigger_waiting{0};
    uint32_t m_chns = 0;
    uint32_t m_cur_t = 0;
    uint64_t m_output_cnt = 0; // in unit of 8 samples. COME BACK TO THIS TOO.
    const double m_step_t;
    const Cmd *m_cmd_read_ptr = nullptr;
    uint32_t m_cmd_read = 0;
    uint32_t m_cmd_max_read = 0;
    std::atomic<uint64_t> &m_cmd_underflow;
    std::atomic<uint64_t> &m_undeflow;
    // Members accessed by the command generation thread
    Cmd *m_cmd_write_ptr __attribute__ ((aligned(64))) = nullptr; //location to write commands to
    uint32_t m_cmd_wrote = 0;
    uint32_t m_cmd_max_write = 0;
    uint32_t m_end_trigger_cnt{0};
    uint32_t m_start_trigger_cnt{0};

    DataPipe<Cmd> m_commands;
    DataPipe<int16_t> m_output;
    std::vector<activeCmd*> active_cmds;
    std::atomic<uint32_t> m_end_triggered{0};
    std::atomic<int64_t> m_time_offset{0};
    // Read by all threads most of the time and
    // may be written by both worker and control threads
    // No ordering is needed on this.
    std::atomic<int16_t*> m_end_trigger{nullptr};
    std::atomic<uint32_t> m_start_trigger{0};
    std::atomic<uint64_t> m_start_trigger_time{0};
};

template<uint32_t max_chns = 128>
struct Stream : StreamBase {
    Stream(double step_t, std::atomic<uint64_t> &cmd_underflow,
           std::atomic<uint64_t> &underflow, bool start=true)
        : StreamBase(step_t, cmd_underflow, underflow)
    {
        m_commands = DataPipe((Cmd*)mapAnonPage(20 * 1024ll, Prot::RW), 1024, 1024);
        m_output = DataPipe((int16_t*)mapAnonPage(4 * 1024ll * 1024ll, Prot::RW), 1024ll * 1024ll);
        if (start) {
            start_worker();
        }
    }

    void start_worker()
    {
        m_stop.store(false, std::memory_order_relaxed);
        m_worker = std::thread(&Stream::thread_fun, this);
    }
    void stop_worker()
    {
        m_stop.store(true, std::memory_order_relaxed);
        if (m_worker.joinable()){
            m_worker.join();
        }
    }
    ~Stream()
    {
        stop_worker();
    }

private:
    void thread_fun()
    {
        /*while (likely(!m_stop.load(std::memory_order_relaxed))) {
            generate_page(m_states);
            }*/
        int[4] outputs = {0, 0, 0, 0};
        while(m_cur_t < 20) {
            std::cout << "m_cur_t=" << m_cur_t << std::endl;
            step(&outputs, m_states);
            std::cout << "amp: ( " << outputs[0] << ", " << outputs[1] << ")" << std::endl;
            std::cout << "freq: ( " << outputs[2] << ", " << outputs[3] << ")" << std::endl;
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
    }
    State m_states[max_chns]{}; // array of states
    std::thread m_worker{};
};

}