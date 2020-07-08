#ifndef DEPENDENCYREACTOR_HPP
#define DEPENDENCYREACTOR_HPP

#include <vector>
#include <deque>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <utility>
#include <unordered_set>

namespace
{


template<typename Context>
struct ContextAssociated
{
    /**
     * Container for Components. Components are owned by this container and only exist once here.
     * std::unique_ptr allows usage of incomplete types.
     */
    template<typename T>
    static T* components;

    template<typename T>
    static std::shared_mutex componentReferenceMutex;
    // this should only prevent threading problems but allow registerComponent to recurse
    // (in ctor of Component another Component can be registered)
    static std::recursive_mutex componentsMutex;

    static std::vector<std::function<void()>> checkDependencies;
    // not shared, because each must only be executed once
    static std::mutex checkDependenciesMutex;

    static std::vector<std::function<std::string()>> whatsMissing;
    static std::mutex whatsMissingMutex;
};

template<typename Context>
template<typename T>
T* ContextAssociated<Context>::components;
template<typename Context>
std::recursive_mutex ContextAssociated<Context>::componentsMutex;

template<typename Context>
std::vector<std::function<void()>> ContextAssociated<Context>::checkDependencies;
template<typename Context>
std::mutex ContextAssociated<Context>::checkDependenciesMutex;

template<typename Context>
std::vector<std::function<std::string()>> ContextAssociated<Context>::whatsMissing;
template<typename Context>
std::mutex ContextAssociated<Context>::whatsMissingMutex;

template<typename Self>
struct SelfAssociated
{
    /**
     * References to all Components reachable from the DependencyReactor, that was not registered by any other
     */
    template<typename T>
    static std::unique_ptr<T> componentsOwned;
};
}

namespace requirecpp {

class DependencyCallbackBase : public std::function<bool()>
{
    public:
    using function::function;
    virtual std::unordered_set<std::string> whatsMissing() const
    {
        return std::unordered_set<std::string>{"Error"};
    }
};

template <typename Context, typename ...Deps>
class DependencyCallback : public DependencyCallbackBase
{
public:
    using DependencyCallbackBase::DependencyCallbackBase;

    std::unordered_set<std::string> whatsMissing() const override
    {
        return _whatsMissing<Deps...>();
    }

    template<typename Dep>
    std::unordered_set<std::string> _whatsMissing() const
    {
        std::unordered_set<std::string> ret{typeid(Dep).name()};
        return ret;
    }

    // exists should be private, is not guarded
    template<typename Dep, typename Dep2, typename ...Args>
    std::unordered_set<std::string> _whatsMissing() const
    {
        auto set1 = _whatsMissing<Dep2, Args...>();
        const auto set2 = _whatsMissing<Dep2, Args...>();
        set1.insert(set2.cbegin(), set2.cend());
        return set1;
    }
};


template<typename T>
class Finished //: public T
{
public:
    using type = T;
    //operator T() const { return *this; }
};

template<typename T>
class Unlocked //: public T
{
public:
    using type = T;
    //operator T() const { return *this; }
};

template<typename Context, typename T>
class ComponentReference
{
    T& m_t;
    std::shared_lock<std::shared_mutex> m_lock;
public:
    using type = T;
    operator T() { return m_t; }
    operator T() const { return m_t; }
    T* operator->() { return &m_t; }
    T* operator->() const { return &m_t;  }
    T& operator* () { return m_t; }
    T& operator* () const { return m_t; }
    ComponentReference(T& t, std::shared_lock<std::shared_mutex>&& lock)
        :m_t{t}
        ,m_lock{std::move(lock)}
    {}
};

// use this to disable debugging information
//template <typename Context, typename ...Deps>
//using CallbackImpl = std::function<bool()>

//template <typename Context, typename ...Deps>
//using CallbackEx = std::function<bool()>

template <typename Context, typename ...Deps>
using CallbackImpl = DependencyCallback<Context, Deps...>;

using CallbackEx = std::unique_ptr<DependencyCallbackBase>;


// Context can be T
// Components have a member DependencyReactor<Themself>
// With that interface executeWith<Dep1, Dep2, Dep3>(lambda) can be used.
// As soon, as all lambdas with dependencies have been executed, onFinished<T>(lambda) is invoked,
// this could happend directly after registerComponent, if all dependecies are met or if there are no dependencies.
// Whenever a component is registered, the executeWith-lambdas that have all dependencies satisfied,
// are executed. After that onFinished is checked an might be invoked.
// executeWith<Dep1, Dep2, Dep3>(lambda) can be used with executeWith<RequireFinished<Dep1>, Dep2, Dep3>(lambda), this must not introduce circular dependencies.
template <typename Context, typename Self = Context>
class DependencyReactor
{
public:
    void checkDependecies()
    {
        auto end1 = std::remove_if(s_executeWith.begin(), s_executeWith.end(), [](auto& cb){ return (*cb)();});
        s_executeWith.erase(end1, s_executeWith.end());
        auto end2 = std::remove_if(m_executeWhenFinished.begin(), m_executeWhenFinished.end(), [](auto& cb){ return (*cb)();});
        m_executeWhenFinished.erase(end2, m_executeWhenFinished.end());
    }
    DependencyReactor()
    {
        std::unique_lock staticLock{s_mutex};
        if(s_exists) throw std::runtime_error{"Only one DependencyReactor per class allowed"};
        s_exists = true;
        staticLock.unlock();
        std::scoped_lock {ContextAssociated<Context>::checkDependenciesMutex};
        ContextAssociated<Context>::checkDependencies.emplace_back(std::bind(&DependencyReactor<Context, Self>::checkDependecies, this));

        std::scoped_lock {ContextAssociated<Context>::whatsMissingMutex};
        ContextAssociated<Context>::whatsMissing.emplace_back(std::bind(&DependencyReactor<Context, Self>::whatsMissingLocal, this));
    }
    DependencyReactor(const DependencyReactor& _other) = delete;
    DependencyReactor& operator=(const DependencyReactor& _other) = delete;

    /**
     *  register compontent of type T. Args are forwarded to construct T
     */
    template<typename T, typename ...Args>
    void createComponent(const Args&& ...args)
    {
        static_assert(!std::is_same<Context, T>(), "Context cannot be registered");

        std::unique_lock contextLock{ContextAssociated<Context>::componentsMutex};

        if(nullptr != ContextAssociated<Context>::template components<T>)
        {
            throw std::runtime_error{"Called registerCompontent twice"};
        }
        if (nullptr == SelfAssociated<Self>::template componentsOwned<T>)
        {
            m_clear.emplace_back([]()
            {
                std::unique_lock{ContextAssociated<Context>::template componentReferenceMutex<T>};
                ContextAssociated<Context>::template components<T> = nullptr;
                SelfAssociated<Self>::template componentsOwned<T>.reset();
            });
        }
        else
        {
            // todo error, exists
            return;
        }
        SelfAssociated<Self>::template componentsOwned<T> = std::make_unique<T>(args...);
        ContextAssociated<Context>::template components<T> = SelfAssociated<Self>::template componentsOwned<T>.get();
        contextLock.unlock();
        std::scoped_lock depsLock{ContextAssociated<Context>::checkDependenciesMutex};
        for (auto&& notif : ContextAssociated<Context>::checkDependencies)
        {
            notif();
        }
    }

    template<typename T>
    void registerExistingComponent(T& comp)
    {
        static_assert(!std::is_same<Context, T>(), "Context cannot be registered");

        std::unique_lock contextLock{ContextAssociated<Context>::componentsMutex};

        if(nullptr != ContextAssociated<Context>::template components<T>)
        {
            throw std::runtime_error{"Called registerCompontent twice"};
        }
        m_clear.emplace_back([]()
        {
            std::unique_lock{ContextAssociated<Context>::template componentReferenceMutex<T>};
            ContextAssociated<Context>::template components<T> = nullptr;
        });
        ContextAssociated<Context>::template components<T> = &comp;
        contextLock.unlock();
        std::scoped_lock depsLock{ContextAssociated<Context>::checkDependenciesMutex};
        for (auto&& notif : ContextAssociated<Context>::checkDependencies)
        {
            notif();
        }
    }


    template <typename ...Tail>
    struct Exists
    {
        static bool exists() { return true;}
    };
    template <typename Dep, typename ...Tail>
    struct Exists<Finished<Dep>, Tail...>
    {
        static bool exists()
        {
            return Exists<Dep>::exists() && DependencyReactor<Context, Dep>::s_executeWith.empty() && Exists<Tail...>::exists();
        }
    };
    template <typename Dep, typename ...Tail>
    struct Exists<Dep, Tail...>
    {
        static bool exists()
        {
            return nullptr != ContextAssociated<Context>::template components<Dep> && Exists<Tail...>::exists();
        }
    };

    // exists should be private, is not guarded
    template<typename ...Args>
    bool exists()
    {
        return Exists<Args...>::exists();
    }


    template <typename T>
    struct Get
    {
        using type = ComponentReference<Context, T>;
        static ComponentReference<Context, T> get()
        {
            std::shared_lock lk{ContextAssociated<Context>::template componentReferenceMutex<T>};
            return {getUnlocked(), std::move(lk)};
        }
        static T& getUnlocked()
        {
            T *&comp = ContextAssociated<Context>::template components<T>;
            if(nullptr == comp) throw std::runtime_error{"Component Unknown"};
            return *comp;
        }
    };
    template <typename T>
    struct Get<Unlocked<T>>
    {
        using type = T&;
        static T& get() { return Get<T>::getUnlocked(); }
        static T& getUnlocked() { return Get<T>::getUnlocked(); }
    };
    template <typename T>
    struct Get<Finished<T>>
    {
        //using type = decltype(Get<T>::get()); // (Get<T>::template type)
        //using get = std::invoke_result_t<Get<T>::get>; // (Get<T>::template type)
        static auto get() { return Get<T>::get(); }
        static T& getUnlocked() { return Get<T>::getUnlocked(); }
    };

    // get should be private
    // Locked/Unlocked must be specified: unlocked is a reference. locked is not
    // callbacks can get a reference to a locked
    // while Finished is omitted in the callback, Unlocked is not. It can be deduced using auto, however, the lambda must dereference (use -> or *)
//    template<typename T>
//    static auto& getUnlocked()
//    {
//        return Get<T>::getUnlocked();
//    }
//    // T can be Finished<T>
//    template<typename T>
//    static auto getLocked() -> decltype(Get<T>::get())
//    {
//        return Get<T>::get();
//    }
    template<typename T>
    static auto require() -> decltype(Get<T>::get())
    {
        return Get<T>::get();
    }

    /**
     *  requireUnlocked can be used when it is impossible, that DependencyReactor<Self> destructs during usage
     */
//    template<typename T>
//    static auto& requireUnlocked()
//    {
//        return Get<T>::get();
//    }

    template<typename T>
    static auto& require()
    {

        //ComponentReference<Context, Get<T>::type> ref{Get<T>::get()};
        return Get<T>::get();
    }

    template<typename ...Deps, typename Callback>
    void require(Callback callback)
    {
        std::unique_lock contextLock{ContextAssociated<Context>::componentsMutex, std::try_to_lock};
        if(contextLock.owns_lock() && exists<Deps...>())
        {
            // avoid copying callback with this extra if
            // if lock could not be acquired, this might be executed in the ctor of a component.
            // in this case, execution is delayed until ctor finished.
            callback(get<Deps>()...);
        }
        else
        {
            auto cb = [this, callback]()
            {
                if(!exists<Deps...>()) return false;
                callback(get<Deps>()...);
                return true;
            };
            std::scoped_lock contextLock{ContextAssociated<Context>::checkDependenciesMutex};
            s_executeWith.emplace_back(std::make_unique<DependencyCallback<Context, Deps...>>(cb));
        }
    }

    template<typename ...Deps, typename Callback>
    void requireFinished(Callback callback)
    {
        std::unique_lock contextLock{ContextAssociated<Context>::componentsMutex, std::try_to_lock};
        if(contextLock.owns_lock() && finished<Deps...>())
        {
            // avoid copying callback with this extra if
            // if lock could not be acquired, this might be executed in the ctor of a component.
            // in this case, execution is delayed until ctor finished.
            callback(get<Deps>()...);
        }
        else
        {
            auto cb = [this, callback]()
            {
                if(!finished<Deps...>()) return false;
                callback(get<Deps>()...);
                return true;
            };
            std::scoped_lock contextLock{ContextAssociated<Context>::checkDependenciesMutex};
            m_executeWhenFinished.emplace_back(std::make_unique<DependencyCallback<Context, Deps...>>(cb));
        }
    }

    static std::string whatsMissing()
    {
        std::scoped_lock {ContextAssociated<Context>::whatsMissingMutex};
        std::string missing{" "};
        for (auto&& notif : ContextAssociated<Context>::whatsMissing)
        {
            missing += notif();
            missing += "\n ";
        }
        if(!missing.empty())
        {
            missing.pop_back();
            missing.pop_back();
        }
        return missing;
    }
    std::string whatsMissingLocal() const
    {
        std::unordered_set<std::string> missing;
        for (const CallbackEx& exec : s_executeWith)
        {
            const auto other = exec->whatsMissing();
            missing.insert(other.cbegin(), other.cend());
        }
        std::string missingConcat;
        for (const auto &miss : missing)
        {
            missingConcat += "\"" + miss + "\"";
            missingConcat += ", ";
        }
        if(!missingConcat.empty())
        {
            missingConcat.pop_back();
            missingConcat.pop_back();
        }

        std::unordered_set<std::string> unfin;
        for (const CallbackEx& exec : m_executeWhenFinished)
        {
            const auto other = exec->whatsMissing();
            unfin.insert(other.cbegin(), other.cend());
        }
        std::string unfinishedConcat;
        for (const auto &u : unfin)
        {
            unfinishedConcat += "\"" + u + "\"";
            unfinishedConcat += ", ";
        }
        if(!unfinishedConcat.empty())
        {
            unfinishedConcat.pop_back();
            unfinishedConcat.pop_back();
        }
        return std::string{"\""} + typeid(Self).name() + "\" needs: " +  missingConcat + "; Not yet finished is: " + unfinishedConcat + ";";
    }

    ~DependencyReactor()
    {
        for (auto&& clear : m_clear)
        {
            clear();
        }
    }
private:

    std::vector<std::function<void()>> m_clear;
    static std::vector<CallbackEx> s_executeWith;
    std::vector<CallbackEx> m_executeWhenFinished;
//    std::vector<std::function<bool()>> m_executeWith;
//    std::vector<std::function<bool()>> m_executeWhenFinished;

    // There must only be one DependencyReactor for each combination of Context and Self
    // using static members DependencyReactor can manage itself in the application without
    // passing references.
    static bool s_exists;
    static std::mutex s_mutex;
    template<typename, typename> friend class DependencyReactor;
};

template <typename Context, typename Self>
bool DependencyReactor<Context, Self>::s_exists = false;

template <typename Context, typename Self>
std::mutex DependencyReactor<Context, Self>::s_mutex{};

template <typename Context, typename Self>
std::vector<CallbackEx> DependencyReactor<Context, Self>::s_executeWith{};

//template <typename Context, typename Self>
//template<typename T>
//T* DependencyReactor<Context, Self>::components = nullptr;

//template <typename Context, typename Self>
//template<typename T>
//std::unique_ptr<T> DependencyReactor<Context, Self>::componentsOwned{};

}

#endif // DEPENDENCYREACTOR_HPP
