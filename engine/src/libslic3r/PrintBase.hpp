#ifndef slic3r_PrintBase_hpp_
#define slic3r_PrintBase_hpp_

#include "WarningCode.hpp"
#include "Config.hpp"
#include "Polygon.hpp"
#include "libslic3r.h"
#include <exception>
#include <cstddef>
#include <cassert>
#include <initializer_list>
#include <set>
#include <utility>
#include <vector>
#include <string>
#include <functional>
#include <atomic>
#include <mutex>

#include "ObjectID.hpp"
#include "Model.hpp"
#include "PlaceholderParser.hpp"
#include "PrintConfig.hpp"

namespace Slic3r {

enum StringExceptionType {
    STRING_EXCEPT_NOT_DEFINED                   = 0,
    STRING_EXCEPT_FILAMENT_NOT_MATCH_BED_TYPE   = 1,
    STRING_EXCEPT_FILAMENTS_DIFFERENT_TEMP      = 2,
    STRING_EXCEPT_OBJECT_COLLISION_IN_SEQ_PRINT = 3,
    STRING_EXCEPT_OBJECT_COLLISION_IN_LAYER_PRINT = 4,
    STRING_EXCEPT_LAYER_HEIGHT_EXCEEDS_LIMIT = 5,
    STRING_EXCEPT_COUNT
};

struct StringObjectException
{
    std::string string;
    ObjectBase const *object = nullptr;
    std::string opt_key;
    StringExceptionType         type = STRING_EXCEPT_NOT_DEFINED;
    bool is_warning = false;
    std::vector<std::string>    params;
    WarningCode code;
};

class CanceledException : public std::exception
{
public:
   const char* what() const throw() { return "Background processing has been canceled"; }
};

class PrintStateBase {
public:
    enum State {
        INVALID,
        STARTED,
        DONE,
    };

    enum class WarningLevel {
        NON_CRITICAL,
        CRITICAL
    };

    enum SlicingNotificationType
    {
        SlicingDefaultNotification = 0,
        SlicingReplaceInitEmptyLayers,
        SlicingNeedSupportOn,
        SlicingEmptyGcodeLayers,
        SlicingGcodeOverlap,
        SlicingPreciseSeamWarning,
    };

    typedef size_t TimeStamp;

    struct StateWithTimeStamp
    {
        StateWithTimeStamp() : state(INVALID), timestamp(0) {}
        State       state;
        TimeStamp   timestamp;
    };

    struct Warning
    {
        WarningLevel    level;
        bool 			current;
        std::string     message;
        int 			message_id;
        WarningCode     code;
    };

    struct StateWithWarnings : public StateWithTimeStamp
    {
    	void 	mark_warnings_non_current() { for (auto &w : warnings) w.current = false; }
        std::vector<Warning>    warnings;
    };

protected:
    static size_t g_last_timestamp;
};

template <class StepType, size_t COUNT>
class PrintState : public PrintStateBase
{
public:
    PrintState() {}

    StateWithTimeStamp state_with_timestamp(StepType step, std::mutex &mtx) const {
        std::scoped_lock<std::mutex> lock(mtx);
        StateWithTimeStamp state = m_state[step];
        return state;
    }

    StateWithWarnings state_with_warnings(StepType step, std::mutex &mtx) const {
        std::scoped_lock<std::mutex> lock(mtx);
        StateWithWarnings state = m_state[step];
        return state;
    }

    bool is_started(StepType step, std::mutex &mtx) const {
        return this->state_with_timestamp(step, mtx).state == STARTED;
    }

    bool is_done(StepType step, std::mutex &mtx) const {
        return this->state_with_timestamp(step, mtx).state == DONE;
    }

    StateWithTimeStamp state_with_timestamp_unguarded(StepType step) const {
        return m_state[step];
    }

    bool is_started_unguarded(StepType step) const {
        return this->state_with_timestamp_unguarded(step).state == STARTED;
    }

    bool is_done_unguarded(StepType step) const {
        return this->state_with_timestamp_unguarded(step).state == DONE;
    }

    template<typename ThrowIfCanceled>
    bool set_started(StepType step, std::mutex &mtx, ThrowIfCanceled throw_if_canceled) {
        std::scoped_lock<std::mutex> lock(mtx);
        throw_if_canceled();
#ifndef NDEBUG
#endif
        if (m_state[step].state == DONE)
            return false;
        PrintStateBase::StateWithWarnings &state = m_state[step];
        state.state = STARTED;
        state.timestamp = ++ g_last_timestamp;
        state.mark_warnings_non_current();
        m_step_active = static_cast<int>(step);
        return true;
    }

	template<typename ThrowIfCanceled>
	std::pair<TimeStamp, bool> set_done(StepType step, std::mutex &mtx, ThrowIfCanceled throw_if_canceled) {
        std::scoped_lock<std::mutex> lock(mtx);
        throw_if_canceled();
        assert(m_state[step].state == STARTED);
        assert(m_step_active == static_cast<int>(step));
        PrintStateBase::StateWithWarnings &state = m_state[step];
        state.state = DONE;
        state.timestamp = ++ g_last_timestamp;
        m_step_active = -1;
    	auto it = std::remove_if(state.warnings.begin(), state.warnings.end(), [](const auto &w) { return ! w.current; });
    	bool update_warning_ui = false;
        if (it != state.warnings.end()) {
        	state.warnings.erase(it, state.warnings.end());
        	update_warning_ui = true;
        }
        return std::make_pair(state.timestamp, update_warning_ui);
    }

    template<typename CancelationCallback>
    bool invalidate(StepType step, CancelationCallback cancel) {
        bool invalidated = m_state[step].state != INVALID;
        if (invalidated) {
#if 0
            if (mtx.state != mtx.HELD) {
                printf("Not held!\n");
            }
#endif
            PrintStateBase::StateWithWarnings &state = m_state[step];
            state.state = INVALID;
            state.timestamp = ++ g_last_timestamp;
            cancel();
            state.mark_warnings_non_current();
            m_step_active = -1;
        }
        return invalidated;
    }

    template<typename CancelationCallback, typename StepTypeIterator>
    bool invalidate_multiple(StepTypeIterator step_begin, StepTypeIterator step_end, CancelationCallback cancel) {
        bool invalidated = false;
        for (StepTypeIterator it = step_begin; it != step_end; ++ it) {
            StateWithTimeStamp &state = m_state[*it];
            if (state.state != INVALID) {
                invalidated = true;
                state.state = INVALID;
                state.timestamp = ++ g_last_timestamp;
            }
        }
        if (invalidated) {
#if 0
            if (mtx.state != mtx.HELD) {
                printf("Not held!\n");
            }
#endif
            cancel();
            for (StepTypeIterator it = step_begin; it != step_end; ++ it)
                m_state[*it].mark_warnings_non_current();
            m_step_active = -1;
        }
        return invalidated;
    }

    template<typename CancelationCallback>
    bool invalidate_all(CancelationCallback cancel) {
        bool invalidated = false;
        for (size_t i = 0; i < COUNT; ++ i) {
            StateWithTimeStamp &state = m_state[i];
            if (state.state != INVALID) {
                invalidated = true;
                state.state = INVALID;
                state.timestamp = ++ g_last_timestamp;
            }
        }
        if (invalidated) {
            cancel();
            for (size_t i = 0; i < COUNT; ++ i)
                m_state[i].mark_warnings_non_current();
            m_step_active = -1;
        }
        return invalidated;
    }

    std::pair<StepType, bool> active_step_add_warning(PrintStateBase::WarningLevel warning_level, const WarningCode &code, const std::string &message, int message_id, std::mutex &mtx)
    {
        std::scoped_lock<std::mutex> lock(mtx);
        assert(m_step_active != -1);
        StateWithWarnings &state = m_state[m_step_active];
        assert(state.state == STARTED);
        std::pair<StepType, bool> retval(static_cast<StepType>(m_step_active), true);
        auto it = (message_id == 0) ?
            std::find_if(state.warnings.begin(), state.warnings.end(), [&message](const auto &w) { return w.message_id == 0 && w.message == message; }) :
            std::find_if(state.warnings.begin(), state.warnings.end(), [message_id](const auto& w) { return w.message_id == message_id; });
        if (it == state.warnings.end())
            state.warnings.emplace_back(PrintStateBase::Warning{ warning_level, true, message, message_id, code });
        else if (it->message != message || it->level != warning_level) {
            it->message = message;
            it->code    = code;
            it->level 	= warning_level;
            it->current = true;
        } else if (it->current)
            retval.second = false;
        else
            it->current = true;
        return retval;
    }

private:
    StateWithWarnings   m_state[COUNT];
    int                 m_step_active = -1;
};

class PrintBase;

class PrintObjectBase : public ObjectBase
{
public:
    const ModelObject*      model_object() const    { return m_model_object; }
    ModelObject*            model_object()          { return m_model_object; }

protected:
    PrintObjectBase(ModelObject *model_object) : m_model_object(model_object) {}
    virtual ~PrintObjectBase() {}
	static std::mutex&                  state_mutex(PrintBase *print);
	static std::function<void()>        cancel_callback(PrintBase *print);
    void status_update_warnings(PrintBase *print, int step, PrintStateBase::WarningLevel warning_level,
        const std::string &message, PrintStateBase::SlicingNotificationType message_id = PrintStateBase::SlicingDefaultNotification);
    void emptylayer_update_msg(PrintBase* print, int type, const std::string& message, bool overwrite);

    ModelObject                  *m_model_object;
};

class PrintTryCancel
{
public:
    void operator()();
private:
    friend PrintBase;
    PrintTryCancel() = delete;
    PrintTryCancel(const PrintBase *print) : m_print(print) {}
    const PrintBase *m_print;
};

class PrintBase : public ObjectBase
{
public:
	PrintBase() : m_placeholder_parser(&m_full_print_config) { this->restart(); }
    inline virtual ~PrintBase() {}

    virtual PrinterTechnology technology() const noexcept = 0;

    virtual void            clear() = 0;
    virtual bool            empty() const = 0;
    virtual std::vector<ObjectID> print_object_ids() const = 0;

    virtual StringObjectException validate(std::vector<StringObjectException> *warnings = nullptr, Polygons* collison_polygons = nullptr, std::vector<std::pair<Polygon, float>>* height_polygons = nullptr) const { return {}; }

    enum ApplyStatus {
        APPLY_STATUS_UNCHANGED,
        APPLY_STATUS_CHANGED,
        APPLY_STATUS_INVALIDATED,
    };
    virtual ApplyStatus     apply(const Model &model, DynamicPrintConfig config, bool extruder_applied = false) = 0;
    const Model&            model() const { return m_model; }

    struct TaskParams {
		TaskParams() : single_model_object(0), single_model_instance_only(false), to_object_step(-1), to_print_step(-1) {}
        ObjectID                single_model_object;
		bool					single_model_instance_only;
        int                     to_object_step;
        int                     to_print_step;
    };
    virtual void            set_task(const TaskParams &params) {}
    virtual void            process(long long *time_cost_with_cache = nullptr, bool use_cache = false) = 0;
    virtual int             export_cached_data(const std::string& dir_path, bool with_space=false) { return 0;}
    virtual int            load_cached_data(const std::string& directory) { return 0;}
    virtual void            finalize() {}

    struct SlicingStatus {
        SlicingStatus(int percent, const std::string &text, unsigned int flags = 0, int warning_step = -1,
            PrintStateBase::SlicingNotificationType  msg_type = PrintStateBase::SlicingDefaultNotification, PrintStateBase::WarningLevel warning_level = PrintStateBase::WarningLevel::NON_CRITICAL) :
            percent(percent), text(text), flags(flags), warning_step(warning_step), message_type(msg_type), warning_level(warning_level)
        {
        }
        SlicingStatus(const PrintBase &print, int warning_step, const std::string& text,
            PrintStateBase::SlicingNotificationType  msg_type = PrintStateBase::SlicingDefaultNotification, PrintStateBase::WarningLevel warning_level = PrintStateBase::WarningLevel::NON_CRITICAL) :
            flags(UPDATE_PRINT_STEP_WARNINGS), warning_object_id(print.id()), text(text), warning_step(warning_step), message_type(msg_type), warning_level(warning_level)
        {
        }
        SlicingStatus(const PrintObjectBase &print_object, int warning_step, const std::string& text,
            PrintStateBase::SlicingNotificationType  msg_type = PrintStateBase::SlicingDefaultNotification, PrintStateBase::WarningLevel warning_level = PrintStateBase::WarningLevel::NON_CRITICAL) :
            flags(UPDATE_PRINT_OBJECT_STEP_WARNINGS), warning_object_id(print_object.id()), text(text), warning_step(warning_step), message_type(msg_type), warning_level(warning_level)
        {
        }
        int             percent { -1 };
        std::string     text;
        enum FlagBits {
            DEFAULT                             = 0,
            RELOAD_SCENE                        = 1 << 1,
            RELOAD_SLA_SUPPORT_POINTS           = 1 << 2,
            RELOAD_SLA_PREVIEW                  = 1 << 3,
            UPDATE_PRINT_STEP_WARNINGS          = 1 << 4,
            UPDATE_PRINT_OBJECT_STEP_WARNINGS   = 1 << 5
        };
        unsigned int    flags;
        ObjectID        warning_object_id;
        int             warning_step { -1 };

        PrintStateBase::SlicingNotificationType  message_type {PrintStateBase::SlicingDefaultNotification};
        PrintStateBase::WarningLevel  warning_level {PrintStateBase::WarningLevel::NON_CRITICAL};
    };
    typedef std::function<void(const SlicingStatus&)>  status_callback_type;
    void                    set_status_default() { m_status_callback = nullptr; }
    void                    set_status_silent() { m_status_callback = [](const SlicingStatus&){}; }
    void                    set_status_callback(status_callback_type cb) { m_status_callback = cb; }
    void                    set_status(int percent, const std::string &message, unsigned int flags = SlicingStatus::DEFAULT, int warning_step = -1) const;

    typedef std::function<void()>  cancel_callback_type;
    void                       set_cancel_callback(cancel_callback_type cancel_callback) { m_cancel_callback = cancel_callback; }
	enum CancelStatus {
		NOT_CANCELED = 0,
		CANCELED_BY_USER = 1,
		CANCELED_INTERNAL = 2
	};
    CancelStatus               cancel_status() const { return m_cancel_status.load(std::memory_order_acquire); }
	bool                       canceled() const { return m_cancel_status.load(std::memory_order_acquire) != NOT_CANCELED; }
	void                       cancel() { m_cancel_status = CANCELED_BY_USER; }
	void                       cancel_internal() { m_cancel_status = CANCELED_INTERNAL; }
	void                       restart() { m_cancel_status = NOT_CANCELED; }
    virtual bool               finished() const = 0;

    const PlaceholderParser&   placeholder_parser() const { return m_placeholder_parser; }
    const DynamicPrintConfig&  full_print_config() const { return m_full_print_config; }

    virtual std::string        output_filename(const std::string &filename_base = std::string()) const = 0;
    std::string                output_filepath(const std::string &path, const std::string &filename_base = std::string()) const;

    int get_plate_index() const { return m_plate_index; }
    void set_plate_index(int index) { m_plate_index = index; }
    bool get_no_check_flag() const { return m_no_check; }
    void set_no_check_flag(bool no_check) { m_no_check = no_check; }

    std::string get_plate_name() const { return m_plate_name; }
    void set_plate_name(const std::string& name) { m_plate_name = name; }
protected:
	friend class PrintObjectBase;
    friend class BackgroundSlicingProcess;

    std::mutex&            state_mutex() const { return m_state_mutex; }
    std::function<void()>  cancel_callback() { return m_cancel_callback; }
	void				   call_cancel_callback() { m_cancel_callback(); }
    void 				   status_update_warnings(int step, PrintStateBase::WarningLevel warning_level,
        const std::string &message, const PrintObjectBase* print_object = nullptr, PrintStateBase::SlicingNotificationType message_id = PrintStateBase::SlicingDefaultNotification);
	void                   status_update_warnings(int step, PrintStateBase::WarningLevel warning_level,
	    const std::string& message, PrintObjectBase &object, PrintStateBase::SlicingNotificationType message_id = PrintStateBase::SlicingDefaultNotification);

    void                   throw_if_canceled() const { if (m_cancel_status.load(std::memory_order_acquire)) throw CanceledException(); }
    PrintTryCancel         make_try_cancel() const { return PrintTryCancel(this); }

    std::string            output_filename(const std::string &format, const std::string &default_ext, const std::string &filename_base, const DynamicConfig *config_override = nullptr) const;
    void                   update_object_placeholders(DynamicConfig &config, const std::string &default_ext) const;

	Model                                   m_model;
	DynamicPrintConfig						m_full_print_config;
    DynamicPrintConfig						m_ori_full_print_config;
    PlaceholderParser                       m_placeholder_parser;

    int m_plate_index{ 0 };
    bool m_no_check = false;

    std::string m_plate_name;

    status_callback_type                    m_status_callback;

private:
    std::atomic<CancelStatus>               m_cancel_status;

    cancel_callback_type                    m_cancel_callback = [](){};

    mutable std::mutex                      m_state_mutex;

    friend PrintTryCancel;
};

template<typename PrintStepEnum, const size_t COUNT>
class PrintBaseWithState : public PrintBase
{
public:
    bool            is_step_done(PrintStepEnum step) const { return m_state.is_done(step, this->state_mutex()); }
	PrintStateBase::StateWithTimeStamp step_state_with_timestamp(PrintStepEnum step) const { return m_state.state_with_timestamp(step, this->state_mutex()); }
    PrintStateBase::StateWithWarnings  step_state_with_warnings(PrintStepEnum step) const { return m_state.state_with_warnings(step, this->state_mutex()); }
    void            active_step_add_warning(PrintStateBase::WarningLevel warning_level, const WarningCode &code, const std::string &message,
                            PrintStateBase::SlicingNotificationType message_id = PrintStateBase::SlicingDefaultNotification)
    {
        std::pair<PrintStepEnum, bool> active_step = m_state.active_step_add_warning(warning_level, code, message, (int)message_id, this->state_mutex());
        if (active_step.second)
            this->status_update_warnings(static_cast<int>(active_step.first), warning_level, message, nullptr, message_id);
    }
protected:
    bool            set_started(PrintStepEnum step) { return m_state.set_started(step, this->state_mutex(), [this](){ this->throw_if_canceled(); }); }
	PrintStateBase::TimeStamp set_done(PrintStepEnum step) {
		std::pair<PrintStateBase::TimeStamp, bool> status = m_state.set_done(step, this->state_mutex(), [this](){ this->throw_if_canceled(); });
        if (status.second)
            this->status_update_warnings(static_cast<int>(step), PrintStateBase::WarningLevel::NON_CRITICAL, std::string());
        return status.first;
	}
    bool            invalidate_step(PrintStepEnum step)
		{ return m_state.invalidate(step, this->cancel_callback()); }
    template<typename StepTypeIterator>
    bool            invalidate_steps(StepTypeIterator step_begin, StepTypeIterator step_end)
        { return m_state.invalidate_multiple(step_begin, step_end, this->cancel_callback()); }
    bool            invalidate_steps(std::initializer_list<PrintStepEnum> il)
        { return m_state.invalidate_multiple(il.begin(), il.end(), this->cancel_callback()); }
    bool            invalidate_all_steps()
        { return m_state.invalidate_all(this->cancel_callback()); }

	bool            is_step_started_unguarded(PrintStepEnum step) const { return m_state.is_started_unguarded(step); }
	bool            is_step_done_unguarded(PrintStepEnum step) const { return m_state.is_done_unguarded(step); }


private:
    PrintState<PrintStepEnum, COUNT> m_state;
};

template<typename PrintType, typename PrintObjectStepEnum, const size_t COUNT>
class PrintObjectBaseWithState : public PrintObjectBase
{
public:
    PrintType*       print()         { return m_print; }
    const PrintType* print() const   { return m_print; }

    typedef PrintState<PrintObjectStepEnum, COUNT> PrintObjectState;
    bool            is_step_done(PrintObjectStepEnum step) const { return m_state.is_done(step, PrintObjectBase::state_mutex(m_print)); }
    PrintStateBase::StateWithTimeStamp step_state_with_timestamp(PrintObjectStepEnum step) const { return m_state.state_with_timestamp(step, PrintObjectBase::state_mutex(m_print)); }
    PrintStateBase::StateWithWarnings  step_state_with_warnings(PrintObjectStepEnum step) const { return m_state.state_with_warnings(step, PrintObjectBase::state_mutex(m_print)); }

protected:
	PrintObjectBaseWithState(PrintType *print, ModelObject *model_object) : PrintObjectBase(model_object), m_print(print) {}

    bool            set_started(PrintObjectStepEnum step)
        { return m_state.set_started(step, PrintObjectBase::state_mutex(m_print), [this](){ this->throw_if_canceled(); }); }
	PrintStateBase::TimeStamp set_done(PrintObjectStepEnum step) {
		std::pair<PrintStateBase::TimeStamp, bool> status = m_state.set_done(step, PrintObjectBase::state_mutex(m_print), [this](){ this->throw_if_canceled(); });
        if (status.second)
            this->status_update_warnings(m_print, static_cast<int>(step), PrintStateBase::WarningLevel::NON_CRITICAL, std::string());
        return status.first;
	}

    bool            invalidate_step(PrintObjectStepEnum step)
        { return m_state.invalidate(step, PrintObjectBase::cancel_callback(m_print)); }
    template<typename StepTypeIterator>
    bool            invalidate_steps(StepTypeIterator step_begin, StepTypeIterator step_end)
        { return m_state.invalidate_multiple(step_begin, step_end, PrintObjectBase::cancel_callback(m_print)); }
    bool            invalidate_steps(std::initializer_list<PrintObjectStepEnum> il)
        { return m_state.invalidate_multiple(il.begin(), il.end(), PrintObjectBase::cancel_callback(m_print)); }
    bool            invalidate_all_steps()
        { return m_state.invalidate_all(PrintObjectBase::cancel_callback(m_print)); }
    bool            invalidate_all_steps_without_cancel()
        { return m_state.invalidate_all([](){}); }

    bool            is_step_started_unguarded(PrintObjectStepEnum step) const { return m_state.is_started_unguarded(step); }
    bool            is_step_done_unguarded(PrintObjectStepEnum step) const { return m_state.is_done_unguarded(step); }

    void            active_step_add_warning(PrintStateBase::WarningLevel warning_level, const WarningCode &code, const std::string &message,
                        PrintStateBase::SlicingNotificationType message_id = PrintStateBase::SlicingDefaultNotification) {
    	std::pair<PrintObjectStepEnum, bool> active_step = m_state.active_step_add_warning(warning_level, code, message,(int) message_id, PrintObjectBase::state_mutex(m_print));
    	if (active_step.second)
    		this->status_update_warnings(m_print, static_cast<int>(active_step.first), warning_level, message, message_id);
    }

protected:
    void            throw_if_canceled() { if (m_print->canceled()) throw CanceledException(); }

    friend PrintType;
    PrintType                               *m_print;

private:
    PrintState<PrintObjectStepEnum, COUNT>   m_state;
};

}

#endif
