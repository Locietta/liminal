#pragma once
#include <cassert>
#include <cstddef>
#include <functional>
#include <new>
#include <type_traits>
#include <utility>

#include <lighter/types.hpp>

namespace lighter {

template <auto V, typename T = decltype(V)>
struct MemFn {
    static_assert(std::is_member_function_pointer_v<T>, "V must be a member Function pointer");
};

template <auto V, typename Class, typename Ret, typename... Args>
    requires std::is_member_function_pointer_v<decltype(V)>
struct MemFn<V, Ret (Class::*)(Args...)> {
    using ClassType = Class;
    using ClassFunctionType = Ret (Class::*)(Args...);
    using FunctionType = Ret(Args...);

    constexpr static ClassFunctionType get() { return V; }
};

template <auto V, typename Class, typename Ret, typename... Args>
    requires std::is_member_function_pointer_v<decltype(V)>
struct MemFn<V, Ret (Class::*)(Args...) const> {
    using ClassType = Class;
    using ClassFunctionType = Ret (Class::*)(Args...) const;
    using FunctionType = Ret(Args...);

    constexpr static ClassFunctionType get() { return V; }
};

template <typename Class, typename MemFn>
concept is_mem_fn_of = requires {
    typename MemFn::ClassType;
    requires std::is_same_v<std::remove_cv_t<Class>, typename MemFn::ClassType>;
};

template <typename Ret, typename Fn, typename... Args>
constexpr Ret invoke_ret(Fn &&fn, Args &&...args) {
    if constexpr (std::is_void_v<Ret>) {
        std::invoke(std::forward<Fn>(fn), std::forward<Args>(args)...);
    } else {
        return std::invoke(std::forward<Fn>(fn), std::forward<Args>(args)...);
    }
}

template <typename Sign>
struct FunctionRef {
    static_assert(false, "Sign must be a Function type");
};

template <typename R, typename... Args>
struct FunctionRef<R(Args...)> {
    using Sign = R(Args...);

    using Erased = union {
        const void *ctx;
        Sign *fn;
    };

    FunctionRef(const FunctionRef &) = default;
    FunctionRef(FunctionRef &&) = default;

    FunctionRef &operator=(const FunctionRef &) = default;
    FunctionRef &operator=(FunctionRef &&) = default;

private:
    constexpr FunctionRef(R (*proxy)(const FunctionRef *, Args &...), Erased ctx) noexcept : proxy{proxy}, erased{ctx} {}

    template <typename Class, typename MemFn, typename ClassType = std::remove_reference_t<Class>>
        requires std::is_lvalue_reference_v<Class &&> && is_mem_fn_of<ClassType, MemFn> &&
                 std::is_invocable_r_v<R, decltype(MemFn::get()), ClassType &, Args...>
    constexpr static FunctionRef make(Class &&invocable, MemFn) noexcept {
        return FunctionRef(
            [](const FunctionRef *self, Args &...args) -> R {
                auto &fn = *const_cast<ClassType *>(static_cast<const ClassType *>(self->erased.ctx));
                return invoke_ret<R>(MemFn::get(), fn, static_cast<Args &&>(args)...);
            },
            Erased{.ctx = &invocable});
    }

    constexpr static FunctionRef make(Sign *invocable) noexcept {
        return FunctionRef(
            [](const FunctionRef *self, Args &...args) -> R {
                Sign *fn = self->erased.fn;
                return (*fn)(static_cast<Args &&>(args)...);
            },
            Erased{.fn = invocable});
    }

    template <typename Class>
    constexpr static FunctionRef make(Class &&invocable) {
        if constexpr (std::is_convertible_v<Class &&, Sign *>) {
            return make(static_cast<Sign *>(std::forward<Class>(invocable)));
        } else {
            using ClassType = std::remove_reference_t<Class>;
            return FunctionRef(
                [](const FunctionRef *self, Args &...args) -> R {
                    auto &fn = *const_cast<ClassType *>(static_cast<const ClassType *>(self->erased.ctx));
                    return invoke_ret<R>(fn, static_cast<Args &&>(args)...);
                },
                Erased{.ctx = &invocable});
        }
    }

public:
    template <auto MemFnPointer, typename Class, typename Mem>
        requires std::is_lvalue_reference_v<Class &&>
    friend constexpr FunctionRef<typename Mem::FunctionType> bind_ref(Class &&obj);

    constexpr FunctionRef(Sign *invocable) noexcept : FunctionRef(make(invocable)) {}

    template <typename Class>
        requires(!std::is_same_v<std::remove_cvref_t<Class>, FunctionRef>) && std::is_lvalue_reference_v<Class &&> &&
                std::is_invocable_r_v<R, Class, Args...>
    constexpr FunctionRef(Class &&invocable) noexcept : FunctionRef(make(std::forward<Class>(invocable))) {}

    template <typename... CallArgs>
    constexpr R operator()(CallArgs &&...args) const {
        static_assert(
            requires(Sign *fn, CallArgs &&...call_args) { fn(std::forward<CallArgs>(call_args)...); },
            "invocable object must be callable with the given arguments");
        return proxy(this, args...);
    }

private:
    R (*proxy)(const FunctionRef *, Args &...);
    Erased erased;
};

template <typename Sign>
struct Function {
    static_assert(false, "Sign must be a Function type");
};

template <typename R, typename... Args>
struct Function<R(Args...)> {
    using Sign = R(Args...);

    using Erased = union {
        void *ctx;
        Sign *fn;
    };

    using Deleter = void(Function *);

    constexpr static usize k_sbo_size = 24;
    constexpr static usize k_sbo_align = alignof(std::max_align_t);

    using Storage = union {
        alignas(k_sbo_align) byte sbo[k_sbo_size];
        Erased erased;
    };

    struct Vtable {
        R (*proxy)(Function *, Args &...);
        Deleter *deleter;
    };

    template <typename T>
    // Use a proper trivially-relocatable trait here once the language provides one.
    constexpr static bool k_sbo_eligible = sizeof(T) <= k_sbo_size && alignof(T) <= k_sbo_align && std::is_trivially_copyable_v<T>;

    Function(const Function &) = delete;

    constexpr Function(Function &&other) noexcept {
        this->vptr = std::exchange(other.vptr, nullptr);
        this->storage = std::exchange(other.storage, Storage{});
    }

    Function &operator=(const Function &) = delete;

    constexpr Function &operator=(Function &&other) noexcept {
        if (this == &other) {
            return *this;
        }
        this->~Function();
        return *new (this) Function(std::move(other));
    }

    constexpr ~Function() {
        if (vptr && vptr->deleter) {
            vptr->deleter(this);
        }
    }

private:
    constexpr Function(const Vtable *vptr, Storage storage = {}) noexcept : storage{storage}, vptr{vptr} {}

    constexpr static Function make(Sign *invocable) noexcept {
        constexpr static Vtable vt = {
            [](Function *self, Args &...args) -> R {
                Sign *fn = self->storage.erased.fn;
                return (*fn)(static_cast<Args &&>(args)...);
            },
            nullptr // No-op deleter for raw Function pointers
        };
        return Function(&vt, Storage{.erased = Erased{.fn = invocable}});
    }

    template <typename Class, typename MemFn, typename ClassType = std::remove_cvref_t<Class>>
        requires k_sbo_eligible<ClassType> && is_mem_fn_of<ClassType, MemFn>
    constexpr static Function make(Class &&invocable, MemFn) {
        if consteval {
            constexpr static Vtable vt = {[](Function *self, Args &...args) -> R {
                                              return (static_cast<ClassType *>(self->storage.erased.ctx)->*MemFn::get())(
                                                  static_cast<Args &&>(args)...);
                                          },
                                          [](Function *self) { delete static_cast<ClassType *>(self->storage.erased.ctx); }};

            return Function(&vt, Storage{.erased = Erased{.ctx = new ClassType(std::forward<Class>(invocable))}});
        } else {
            constexpr static Vtable vt = {[](Function *self, Args &...args) -> R {
                                              return (self->storage_as<ClassType>()->*MemFn::get())(static_cast<Args &&>(args)...);
                                          },
                                          [](Function *self) { self->storage_as<ClassType>()->~ClassType(); }};
            Storage storage{};
            new (storage.sbo) ClassType(std::forward<Class>(invocable));
            return Function(&vt, storage);
        }
    }

    template <typename Class, typename MemFn, typename ClassType = std::remove_cvref_t<Class>>
        requires(!k_sbo_eligible<ClassType>) && is_mem_fn_of<ClassType, MemFn>
    constexpr static Function make(Class &&invocable, MemFn) {
        constexpr static Vtable vt = {[](Function *self, Args &...args) -> R {
                                          return (static_cast<ClassType *>(self->storage.erased.ctx)->*MemFn::get())(
                                              static_cast<Args &&>(args)...);
                                      },
                                      [](Function *self) { delete static_cast<ClassType *>(self->storage.erased.ctx); }};

        return Function(&vt, Storage{.erased = Erased{.ctx = new ClassType(std::forward<Class>(invocable))}});
    }

    template <typename Class>
    constexpr static Function make(Class &&invocable) {
        if constexpr (std::is_convertible_v<Class &&, Sign *>) {
            return make(static_cast<Sign *>(std::forward<Class>(invocable)));
        } else {
            using ClassType = std::remove_cvref_t<Class>;
            if constexpr (k_sbo_eligible<ClassType>) {
                if consteval {
                    constexpr static Vtable vt = {[](Function *self, Args &...args) -> R {
                                                      auto &fn = *static_cast<ClassType *>(self->storage.erased.ctx);
                                                      return invoke_ret<R>(fn, static_cast<Args &&>(args)...);
                                                  },
                                                  [](Function *self) { delete static_cast<ClassType *>(self->storage.erased.ctx); }};

                    return Function(&vt, Storage{.erased = Erased{.ctx = new ClassType(std::forward<Class>(invocable))}});
                } else {
                    constexpr static Vtable vt = {[](Function *self, Args &...args) -> R {
                                                      auto &fn = *self->storage_as<ClassType>();
                                                      return invoke_ret<R>(fn, static_cast<Args &&>(args)...);
                                                  },
                                                  [](Function *self) { self->storage_as<ClassType>()->~ClassType(); }};
                    Storage storage{};
                    new (storage.sbo) ClassType(std::forward<Class>(invocable));
                    return Function(&vt, storage);
                }
            } else {
                constexpr static Vtable vt = {[](Function *self, Args &...args) -> R {
                                                  auto &fn = *static_cast<ClassType *>(self->storage.erased.ctx);
                                                  return invoke_ret<R>(fn, static_cast<Args &&>(args)...);
                                              },
                                              [](Function *self) { delete static_cast<ClassType *>(self->storage.erased.ctx); }};

                return Function(&vt, Storage{.erased = Erased{.ctx = new ClassType(std::forward<Class>(invocable))}});
            }
        }
    }

public:
    template <auto MemFnPointer, typename Class, typename Mem>
    friend constexpr Function<typename Mem::FunctionType> bind(Class &&obj);

    template <typename Class>
        requires(!std::is_same_v<std::remove_cvref_t<Class>, Function>) && std::is_invocable_r_v<R, Class, Args...>
    constexpr Function(Class &&invocable) : Function(make(std::forward<Class>(invocable))) {}

    template <typename... CallArgs>
    constexpr R operator()(CallArgs &&...args) {
        static_assert(
            requires(Sign *fn, CallArgs &&...call_args) { fn(std::forward<CallArgs>(call_args)...); },
            "invocable object must be callable with the given arguments");
        assert(vptr && "Attempting to call an empty Function object");
        return vptr->proxy(this, args...);
    }

private:
    template <typename Class>
    const Class *storage_as() const {
        return std::launder(reinterpret_cast<const Class *>(this->storage.sbo));
    }

    template <typename Class>
    Class *storage_as() {
        return std::launder(reinterpret_cast<Class *>(this->storage.sbo));
    }

    Storage storage;
    const Vtable *vptr;
};

template <typename R, typename... Args>
struct Function<R(Args...) const> {
    using Sign = R(Args...);

    using Erased = union {
        const void *ctx;
        Sign *fn;
    };

    using Deleter = void(Function *);

    constexpr static usize k_sbo_size = 24;
    constexpr static usize k_sbo_align = alignof(std::max_align_t);

    using Storage = union {
        alignas(k_sbo_align) byte sbo[k_sbo_size];
        Erased erased;
    };

    struct Vtable {
        R (*proxy)(const Function *, Args &...);
        Deleter *deleter;
    };

    template <typename T>
    // Use a proper trivially-relocatable trait here once the language provides one.
    constexpr static bool k_sbo_eligible = sizeof(T) <= k_sbo_size && alignof(T) <= k_sbo_align && std::is_trivially_copyable_v<T>;

    Function(const Function &) = delete;

    constexpr Function(Function &&other) noexcept {
        this->vptr = std::exchange(other.vptr, nullptr);
        this->storage = std::exchange(other.storage, Storage{});
    }

    Function &operator=(const Function &) = delete;

    constexpr Function &operator=(Function &&other) noexcept {
        if (this == &other) {
            return *this;
        }
        this->~Function();
        return *new (this) Function(std::move(other));
    }

    constexpr ~Function() {
        if (vptr && vptr->deleter) {
            vptr->deleter(this);
        }
    }

private:
    constexpr Function(const Vtable *vptr, Storage storage = {}) noexcept : storage{storage}, vptr{vptr} {}

    constexpr static Function make(Sign *invocable) noexcept {
        constexpr static Vtable vt = {
            [](const Function *self, Args &...args) -> R {
                Sign *fn = self->storage.erased.fn;
                return (*fn)(static_cast<Args &&>(args)...);
            },
            nullptr // No-op deleter for raw Function pointers
        };
        return Function(&vt, Storage{.erased = Erased{.fn = invocable}});
    }

    template <typename Class, typename MemFn, typename ClassType = std::remove_cvref_t<Class>>
        requires k_sbo_eligible<ClassType> && is_mem_fn_of<ClassType, MemFn> &&
                 std::is_invocable_r_v<R, decltype(MemFn::get()), const ClassType &, Args...>
    constexpr static Function make(Class &&invocable, MemFn) {
        if consteval {
            constexpr static Vtable vt = {[](const Function *self, Args &...args) -> R {
                                              return (static_cast<const ClassType *>(self->storage.erased.ctx)->*MemFn::get())(
                                                  static_cast<Args &&>(args)...);
                                          },
                                          [](Function *self) { delete static_cast<const ClassType *>(self->storage.erased.ctx); }};

            return Function(&vt, Storage{.erased = Erased{.ctx = new ClassType(std::forward<Class>(invocable))}});
        } else {
            constexpr static Vtable vt = {[](const Function *self, Args &...args) -> R {
                                              return (self->storage_as<ClassType>()->*MemFn::get())(static_cast<Args &&>(args)...);
                                          },
                                          [](Function *self) { self->storage_as<ClassType>()->~ClassType(); }};
            Storage storage{};
            new (storage.sbo) ClassType(std::forward<Class>(invocable));
            return Function(&vt, storage);
        }
    }

    template <typename Class, typename MemFn, typename ClassType = std::remove_cvref_t<Class>>
        requires(!k_sbo_eligible<ClassType>) && is_mem_fn_of<ClassType, MemFn> &&
                std::is_invocable_r_v<R, decltype(MemFn::get()), const ClassType &, Args...>
    constexpr static Function make(Class &&invocable, MemFn) {
        constexpr static Vtable vt = {[](const Function *self, Args &...args) -> R {
                                          return (static_cast<const ClassType *>(self->storage.erased.ctx)->*MemFn::get())(
                                              static_cast<Args &&>(args)...);
                                      },
                                      [](Function *self) { delete static_cast<const ClassType *>(self->storage.erased.ctx); }};

        return Function(&vt, Storage{.erased = Erased{.ctx = new ClassType(std::forward<Class>(invocable))}});
    }

    template <typename Class>
    constexpr static Function make(Class &&invocable) {
        if constexpr (std::is_convertible_v<Class &&, Sign *>) {
            return make(static_cast<Sign *>(std::forward<Class>(invocable)));
        } else {
            using ClassType = std::remove_cvref_t<Class>;
            if constexpr (k_sbo_eligible<ClassType>) {
                if consteval {
                    constexpr static Vtable vt = {[](const Function *self, Args &...args) -> R {
                                                      auto &fn = *static_cast<const ClassType *>(self->storage.erased.ctx);
                                                      return invoke_ret<R>(fn, static_cast<Args &&>(args)...);
                                                  },
                                                  [](Function *self) { delete static_cast<const ClassType *>(self->storage.erased.ctx); }};

                    return Function(&vt, Storage{.erased = Erased{.ctx = new ClassType(std::forward<Class>(invocable))}});
                } else {
                    constexpr static Vtable vt = {[](const Function *self, Args &...args) -> R {
                                                      auto &fn = *self->storage_as<ClassType>();
                                                      return invoke_ret<R>(fn, static_cast<Args &&>(args)...);
                                                  },
                                                  [](Function *self) { self->storage_as<ClassType>()->~ClassType(); }};
                    Storage storage{};
                    new (storage.sbo) ClassType(std::forward<Class>(invocable));
                    return Function(&vt, storage);
                }
            } else {
                constexpr static Vtable vt = {[](const Function *self, Args &...args) -> R {
                                                  auto &fn = *static_cast<const ClassType *>(self->storage.erased.ctx);
                                                  return invoke_ret<R>(fn, static_cast<Args &&>(args)...);
                                              },
                                              [](Function *self) { delete static_cast<const ClassType *>(self->storage.erased.ctx); }};

                return Function(&vt, Storage{.erased = Erased{.ctx = new ClassType(std::forward<Class>(invocable))}});
            }
        }
    }

public:
    template <typename Class>
        requires(!std::is_same_v<std::remove_cvref_t<Class>, Function>) &&
                std::is_invocable_r_v<R, const std::remove_reference_t<Class> &, Args...>
    constexpr Function(Class &&invocable) : Function(make(std::forward<Class>(invocable))) {}

    template <typename... CallArgs>
    constexpr R operator()(CallArgs &&...args) const {
        static_assert(
            requires(Sign *fn, CallArgs &&...call_args) { fn(std::forward<CallArgs>(call_args)...); },
            "invocable object must be callable with the given arguments");
        assert(vptr && "Attempting to call an empty Function object");
        return vptr->proxy(this, args...);
    }

private:
    template <typename Class>
    const Class *storage_as() const {
        return std::launder(reinterpret_cast<const Class *>(this->storage.sbo));
    }

    template <typename Class>
    Class *storage_as() {
        return std::launder(reinterpret_cast<Class *>(this->storage.sbo));
    }

    Storage storage;
    const Vtable *vptr;
};

template <auto MemFnPointer, typename Class, typename Mem = MemFn<MemFnPointer>>
    requires std::is_lvalue_reference_v<Class &&>
constexpr FunctionRef<typename Mem::FunctionType> bind_ref(Class &&obj) {
    return FunctionRef<typename Mem::FunctionType>::make(std::forward<Class>(obj), Mem{});
}

template <auto MemFnPointer, typename Class, typename Mem = MemFn<MemFnPointer>>
constexpr Function<typename Mem::FunctionType> bind(Class &&obj) {
    return Function<typename Mem::FunctionType>::make(std::forward<Class>(obj), Mem{});
}

} // namespace lighter
