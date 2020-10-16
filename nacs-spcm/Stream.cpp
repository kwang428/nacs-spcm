//Written by Kenneth Wang in Oct 2020

#include "Stream.h"

#include <nacs-spcm/spcm.h>
#include <nacs-utils/log.h>
#include <nacs-utils/mem.h>
#include <nacs-utils/thread.h>

#include <sys/stat.h>
#include <fcntl.h>
#include <math.h>
#include <cstdlib>
#include <cmath>

#include <immintrin.h>
#include <sleef.h>

using namespace NaCs;

namespace Spcm{

template<typename T>
static NACS_INLINE void accum_nonzero(T &out, T in, float s)
{
    if (__builtin_constant_p(s) && s == 0)
        return;
    out += in * s;
}

constexpr long long int sample_rate = 625ll * 1000000ll;
constexpr int cycle = 1024/32;

constexpr uint64_t max_phase = uint64_t(625e6 * 10);
constexpr double phase_scale = 2 / double(max_phase);
constexpr double freq_scale = 0.1 / (625e6 / 32); // 1 cycle in 32 samples at 625 MHz sampling rate. Converts a frequency at 10 times the real frequency, hence the 0.1.


//__m512 is a vector type that can hold 16 32 bit floats
static constexpr __m512 tidxs = {0.0, 0.0625, 0.125, 0.1875, 0.25, 0.3125, 0.375, 0.4375,
                                 0.5, 0.5625, 0.625, 0.6875, 0.75, 0.8125, 0.875, 0.9375};


typedef short v32si __attribute__((vector_size(64)));
static constexpr v32si mask0 = {1, 3, 5, 7, 9, 11, 13, 15,
                                17, 19, 21, 23, 25, 27, 29, 31,
                                33, 35, 37, 39, 41, 43, 45, 47,
                                49, 51, 53, 55, 57, 59, 61, 63};
// d is a vector of phases
__attribute__((target("avx512f,avx512bw"), flatten))
__m512 xsinpif_pi(__m512 d)
{
    __m512i q = _mm512_cvtps_epi32(d);
    d = d - _mm512_cvtepi32_ps(q);

    __m512 s = d * d;

    auto neg = _mm512_test_epi32_mask(q, _mm512_set1_epi32(1));
    d = (__m512)_mm512_mask_xor_epi32((__m512i)d, neg, (__m512i)d, _mm512_set1_epi32(0x80000000));

    auto u = 0.024749093f * s - 0.19045785f;
    u = u * s + 0.8117177f;
    u = u * s - 1.6449335f;
    return (s * d) * u + d;
}

// Amplitude normalized to 6.7465185f9 (2^31 - 1) * pi
// Phase is in unit of pi
// Frequency of 1 means one full cycle per 32 samples. At 625 MHz sampling rate, this is 19.531250 MHz
__attribute__((target("avx512f,avx512bw"), flatten))
void compute_single_chn(__m512 &v1, __m512 &v2, float phase, float freq,
                        float df, float amp, float damp)
{
    auto phase_v1 = phase + freq * tidxs; // first 16 samples
    auto phase_v2 = phase + freq * (tidxs + 1); // next 16 samples with no df
    accum_nonzero(phase_v1, tidxs, df / 2);
    accum_nonzero(phase_v2, (tidxs + 1), df / 2);
    auto amp_v1 = _mm512_set1_ps(amp);
    auto amp_v2 = _mm512_set1_ps(amp + damp / 2);
    accum_nonzero(amp_v1, tidxs, damp / 2); // accumulate half amplitude in one go
    accum_nonzero(amp_v2, tidxs, damp / 2); // accumulate next half
    v1 += xsinpif_pi(phase_v1) * amp_v1;
    v2 += xsinpif_pi(phase_v2) * amp_v2;
}

void test_compute_single_chn(int& out, int& out2, float val, float dval) {
    out1 += val;
    out2 = out2 + val + dval;
}

NACS_EXPORT() const char *Cmd::name() const
{
    // gives the name of the Cmd
    switch(op()){
    case CmdType::AmpSet:
        return "ampSet";
    case CmdType::AmpFn:
        return "ampFn";
    case CmdType::AmpVecFn:
        return "ampVecFn";
    case CmdType::FreqSet:
        return "freqSet";
    case CmdType::FreqFn:
        return "freqFn";
    case CmdType::FreqVecFn:
        return "freqVecFn";
    case CmdType::Phase:
        return "phase";
    case CmdType::ModChn:
        if (chn == add_chn)
            return "add_chn";
        return "del_chn";
    case CmdType::Meta:
        if (chn == (uint32_t)CmdMeta::Reset)
            return "reset";
        if (chn == (uint32_t)CmdMeta::ResetAll)
            return "reset_all";
        if (chn == (uint32_t)CmdMeta::TriggerEnd)
            return "trigger_end";
        if (chn == (uint32_t)CmeMeta::TriggerStart)
            return "trigger_start";
    default:
        return "(unknown)";
    }
}

NACS_EXPORT() std::ostream &operator<<(std::ostream &stm, const Cmd &cmd)
{
    // defines how a command behaves with std::cout << Cmd for instance
    stm << cmd.name() << "(t =" << cmd.t;
    if (cmd.op() == CmdType::Meta &&
        (cmd.chn == (uint32_t)CmdMeta::TriggerEnd || cmd.chn == (uint32_t)CmdMeta::TriggerStart))
        stm << ", id=" << cmd.final_val;
    if (cmd.op() == CmdType::ModChn && cmd.chn != Cmd::add_chn)
        stm << ", chn=" << cmd.chn;
    if (cmd.op() == CmdType::FreqSet || cmd.op() == CmdType::AmpSet || cmd.op() == CmdType::Phase)
        stm << ", chn=" << cmd.chn << ", val=" << cmd.final_val;
    if (cmd.op() == CmdType::AmpFn || cmd.op() == CmdType::FreqFn ||
        cmd.op() == CmdType::AmpVecFn || cmd.op() == CmdType::FreqVecFn)
        stm << ", chn=" << cmd.chn << ", final_val=" << cmd.final_val << ", len=" << cmd.len;
    stm << ")";
    return stm;
}

NACS_EXPORT() void Cmd::dump() const
{
    std::cerr << *this << std::endl;
}

NACS_EXPORT() std::ostream &operator<<(std::ostream &stm, const std::vector<Cmd> &cmds)
{
    for (auto &cmd: cmds)
        stm << cmd << std::endl;
    return stm;
}

std::pair<int32_t, int32_t> activeCmd::eval(uint32_t t) {
    // t is time from beginning of pulse
    int32_t val, dval;
    if (m_cmd->op() == CmdType::AmpVecFn || m_cmd->op() == CmdType::FreqVecFn) {
        // assume all values are precalculated
        val = vals[t];
        dval = vals[t + 1] - vals[t];
    }
    else if (m_cmd->op() == CmdType::AmpFn || m_cmd->op() == CmdType::FreqFn) {
        // check if t and t + 1 is evaluated
        while (vals.size() < (t + 2)) {
            int32_t thisval;
            thisval = ((int32_t(*)(uint32_t))(m_cmd->fnptr))((uint32_t) vals.size());
            vals.push_back(thisval);
        }
        val = vals[t];
        dval = vals[t + 1] - vals[t];
    }
    else {
        val = 0;
        dval = 0; // default behavior
    }
    return std::make_pair(val, dval);
}

NACS_INLINE void StreamBase::clear_underflow()
{
    m_cmd_underflow.store(0, std::memory_order_relaxed);
    m_underflow.store(0, std::memory_order_relaxed);
}

inline const Cmd *StreamBase::get_cmd_curt()
{
    // check get_cmd returns something valid and if so is t less than the current time
    if (auto cmd = get_cmd()){
        if (cmd->t <= m_cur_t) {
            return cmd;
        }
    }
    return nullptr;
}

inline const Cmd *StreamBase::get_cmd()
{
    // returns command at m_cmd_read location. If it's hit max, reset to zero and get a new pointer
    if (m_cmd_read == m_cmd_max_read) {
        m_cmd_read = 0;
        m_cmd_read_ptr = m_commands.get_read_ptr(m_cmd_max_read);
        // check if m_cmd_max_read == 0
        if (!m_cmd_max_read) {
            return nullptr;
        }
    }
    return &m_cmd_read_ptr[m_cmd_read];
}

inline void StreamBase::cmd_next()
{
    // increment m_cmd_read in the if statement. If hit max_read, alert writer that reading is done
    if (++m_cmd_read == m_cmd_max_read) {
        m_commands.read_size(m_cmd_max_read);
    }
}

// TRIGGER STUFF. COME BACK TO
inline bool StreamBase::check_start(uint32_t t, uint32_t id)
{
    // The corresponding time must be visible when the id is loaded
    // We don't load the time and the id atomically so it is possible
    // that the time could be the next trigger.
    // It is highly unlikely and we should never have that
    // situation in real experiment.
    // If it really happens, we'll simply wait until the corresponding
    // trigger id to be visible too.
    m_cur_t = t;
    if (m_start_trigger.load(std::memory_order_acquire) < id)
        goto not_yet;
    {
        auto global_time = uint64_t(m_step_t * m_output_cnt);
        auto trigger_time =
            m_start_trigger_time.load(std::memory_order_relaxed);
        if (time_offset() + global_time < trigger_time) {
            goto not_yet;
        }
    }
    m_slow_mode.store(false, std::memory_order_relaxed);
    return true;
not_yet:
    m_slow_mode.store(true, std::memory_order_relaxed);
    return false;
}

NACS_INTERNAL NACS_NOINLINE const Cmd*
StreamBase::consume_old_cmds(State *states)
{
    // consumes old commands (updates the states) and returns a pointer to a currently active command.
    // If only commmands in future or no commands, then return nullptr
    auto cmd = get_cmd();
    if (cmd->t != 0)
        m_cmd_underflow.fetch_add(1, std::memory_order_relaxed);
    do {
        if (cmd->t == m_cur_t)
            return cmd;
        if (cmd->t > m_cur_t)
            return nullptr; // get_cmd returns something in the future
        switch (cmd->op()){
        case CmdType::Meta:
            if (cmd->chn == (uint32_t)CmdMeta::Reset) {
                m_cur_t = 0; // set time to 0 if consuming a Reset
            }
            else if (cmd->chn == (uint32_t)CmdMeta::ResetAll){
                clear_underflow();
                m_cur_t = 0;
                m_chns = 0;
                m_slow_mode.stsore(false,std::memory_order_relaxed);
            }
            else if (cmd-> chn == (uint32_t)CmdMeta::TriggerEnd) {
                m_end_trigger_pending = cmd->final_val;
            }
            else if (cmd-> chn == (uint32_t)CmdMeta::TriggerStart) {
                if (!check_start(cmd->t, cmd->final_val)) {
                    return nullptr;
                }
            }
            break;
        case CmdType::AmpSet:
            states[cmd->chn].amp = cmd->final_val; // set amplitude of state
            break;
        case CmdType::FreqSet:
            states[cmd->chn].freq = cmd->final_val;
            break;
        case CmdType::AmpFn:
        case CmdType::AmpVecFn:
            // cmd pointer only increments. Should be safe to initialize an active command here
            if (cmd->t + len > m_cur_t) {
                // command still active
                active_cmds.emplace_back(cmd);
                std::pair<int32_t, int32_t> these_vals;
                these_vals = active_cmds.back().eval(m_cur_t - cmd->t);
                states[cmd->chn].amp = these_vals.first() + these_vals.second();
            }
            else {
                states[cmd->chn].amp = cmd->final_val; // otherwise set to final value.
            }
            break;
        case CmdType::FreqFn:
        case CmdType::FreqVecFn:
            if (cmd->t + len > m_cur_t) {
                // command still active
                active_cmds.emplace_back(cmd);
                std::pair<int32_t, int32_t> these_vals;
                these_vals = active_cmds.back().eval(m_cur_t - cmd->t);
                states[cmd->chn].freq = these_vals.first() + these_vals.second();
            }
            else {
                states[cmd->chn].freq = cmd->final_val; // otherwise set to final value.
            }
            break;
        case CmdType::Phase:
            states[cmd->chn].phase = cmd->final_val; // possibly a scale factor needed. COME BACK
            break;
        case CmdType::ModChn:
            if (cmd->chn == Cmd::add_chn) {
                states[m_chns] = {0, 0, 0}; // initialize new channel
                m_chns++;
            }
            else {
                m_chns--;
                states[cmd->chn] = states[m_chns]; // move last_chn to place of deleted channel
            }
            break;
        }
        cmd_next(); //after interpretting this command, increment pointer to next one.
    } while((cmd = get_cmd())); // keep on going until there are no more commands or one reaches the present
    return nullptr;
}

NACS_INLINE void StreamBase::step(int *out, State *states)
{
    // Key function
    const Cmd *cmd;
retry:
    // returns command at current time or before
    if ((cmd = get_cmd_curt())){
        if (unlikely(cmd->t < m_cur_t)) {
            cmd = consume_old_cmds(states); //consume past commands
            if (!cmd) {
                goto cmd_out; //if no command available, go to cmd_out
            }
        }
        if (cmd_t > m_cur_t) {
            cmd = nullptr; // don't deal with future commands
        }
        // deal with different types of commands
        else if (unlikely(cmd->op() == CmdType::Meta)) {
            if (cmd->chn == (uint32_t)CmdMeta::Reset) {
                m_cur_t = 0;
            }
            else if (cmd->chn == (uint32_t)CmdMeta::ResetAll) {
                clear_underflow();
                m_cur_t = 0;
                m_chns = 0; 
                m_slow_mode.store(false, std::memory_order_relaxed);
            }
            else if (cmd->chn == (uint32_t)CmdMeta::TriggerEnd) {
                m_end_trigger_pending = cmd->i;
            }
            else if (cmd->chn == (uint32_t)CmdMeta::TriggerStart) {
                if (!check_start(cmd->t, cmd->i)){
                    cmd = nullptr;
                    goto cmd_out;
                }
            }
            cmd_next();
            goto retry; // keep on going if it's a meta command
        }
        else {
            while (unlikely(cmd->op() == CmdType::ModChn)) {
                if (cmd->chn == Cmd::add_chn) {
                    states[m_chns] = {0, 0, 0};
                    m_chns++;
                }
                else {
                    m_chns--;
                    states[cmd->chn] = states[m_chns];
                }
                cmd_next();
                cmd = get_cmd_curt();
                if (!cmd) {
                    break;
                } // keep on getting more commands until you're done adding channels.
                // What if you get a meta command here....
            }
        }
    }
cmd_out:
    // At this point we have a nullptr if out of commands or in the future, or it's an actual command
    // related to amp, phase, freq
    if (unlikely(m_end_trigger_waiting)) {
        auto cur_end_trigger = end_trigger();
        if (!cur_end_trigger) {
            m_end_triggered.store(m_end_trigger_waiting, std::memory_order_relaxed);
            m_end_trigger_waiting = m_end_trigger_pending;
            if (m_end_trigger_pending) {
                set_end_trigger(out);
            }
        }
    }
    else if (unlikely(m_end_trigger_pending)) {
        m_end_trigger_waiting = m_end_trigger_pending;
        m_end_trigger_pending = 0;
        set_end_trigger(out);
    }
    // calculate actual output.
    // For testing purposes. At the moment keep the output simple.
    int out1amp, out2amp, out1freq, out2freq;
    uint32_t _nchns = m_chns;
    for (uint32_t i = 0; i < _nchns; i++){
        // iterate through the number of channels
        auto &state = states[i];
        auto phase = state.phase;
        auto amp = state.amp;
        auto freq = state.freq;
        int32_t df = 0;
        int32_t damp = 0;
        // check active commands
        auto it = active_cmds.begin();
        while(it != active_cmds.end()) {
            Cmd* this_cmd = (*it).m_cmd;
            if (this_cmd->chn == i) {
                if (this_cmd->op() == CmdType::AmpFn || this_cmd->op() == CmdType::AmpVecFn) {
                    if (this_cmd->t + len > m_cur_t) {
                        std::pair<int32_t, int32_t> these_vals;
                        these_vals = this_cmd.eval(m_cur_t - this_cmd->t);
                        amp = these_vals.first();
                        damp = these_vals.second();
                    }
                    else {
                        amp = this_cmd->final_val;
                        it = active_cmds.erase(it); // no longer active
                        continue;
                    }
                }
                else if (this_cmd->op() == CmdType::FreqFn || this_cmd->op() == CmdType::FreqVecFn) {
                    if (this_cmd->t + len > m_cur_t) {
                        std::pair<int32_t, int32_t> these_vals;
                        these_vals = this_cmd.eval(m_cur_t - this_cmd->t);
                        freq = these_vals.first();
                        df = these_vals.second();
                    }
                    else {
                        freq = this_cmd->final_val;
                        it = active_cmds.erase(it); // no longer active
                        continue;
                    }
                }
            }
            ++it;
        }
        // now deal with current command
        if (!cmd || cmd->chn != i) {
            test_compute_single_chn(out1amp, out2amp, amp, damp);
            test_compute_single_chn(out1freq, out2freq, freq, df);
        }
        else {
            do {
                if (cmd->op() == CmdType::FreqSet){
                    freq = cmd->final_val;
                }
                else if (cmd->op() == CmdType::FreqFn || cmd->op() == CmdType::FreqVecFn) {
                    // first time seeing function command
                    if (cmd->t + len > m_cur_t) {
                        // command still active
                        active_cmds.emplace_back(cmd);
                        std::pair<int32_t, int32_t> these_vals;
                        these_vals = active_cmds.back().eval(m_cur_t - cmd->t);
                        freq = these_vals.first();
                        df = these_vals.second();
                    }
                    else {
                        freq = cmd->final_val; // otherwise set to final value.
                    }
                }
                else if (likely(cmd->op() == CmdType::AmpFn || cmd->op() == CmdType::AmpVecFn)) {
                    // first time seeing function command
                    if (cmd->t + len > m_cur_t) {
                        // command still active
                        active_cmds.emplace_back(cmd);
                        std::pair<int32_t, int32_t> these_vals;
                        these_vals = active_cmds.back().eval(m_cur_t - cmd->t);
                        amp = these_vals.first();
                        damp = these_vals.second();
                    }
                    else {
                        amp = cmd->final_val; // otherwise set to final value.
                    }
                }
                else if (unlikely(cmd->op() == CmdType::Phase)) {
                    phase = cmd->final_val; // may need to be changed
                }
                else {
                    //encountered a non phase,amp,freq command
                    break;
                }
                cmd_next(); // increment cmd counter
                cmd = get_cmd_curt(); // get command only if it's current
            } while (cmd && cmd->chn == i);
            test_compute_single_chn(out1amp, out2amp, amp, damp);
            test_compute_single_chn(out1freq, out2freq, freq, df);
        }
        // deal with phase wraparound
        if (phase > max_phase || phase < -max_phase)
            phase = phase % max_phase;
        state.phase = phase;
    } // channel iteration
    // after done iterating channels
    m_cur_t++; // increment time
    *out = out1amp;
    *(out + 1) = out2amp;
    *(out + 2) = out1freq;
    *(out + 3) = out2freq;
}

NACS_EXPORT() void StreamBase::generate_page(State *states)
{
    
}

}
