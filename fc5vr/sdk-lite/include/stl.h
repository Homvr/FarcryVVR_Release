#pragma once

namespace stl
{

    template <class To, class From>
    [[nodiscard]] To unrestricted_cast(From a_from) noexcept
    {
        if constexpr (std::is_same_v<std::remove_cv_t<From>, std::remove_cv_t<To>>) {
            return To{ a_from };

            // From != To
        } else if constexpr (std::is_reference_v<From>) {
            return stl::unrestricted_cast<To>(std::addressof(a_from));

            // From: NOT reference
        } else if constexpr (std::is_reference_v<To>) {
            return *stl::unrestricted_cast<std::add_pointer_t<std::remove_reference_t<To>>>(a_from);

            // To: NOT reference
        } else if constexpr (std::is_pointer_v<From> && std::is_pointer_v<To>) {
            return static_cast<To>(const_cast<void*>(static_cast<const volatile void*>(a_from)));
        } else if constexpr ((std::is_pointer_v<From> && std::is_integral_v<To>) || (std::is_integral_v<From> && std::is_pointer_v<To>)) {
            return reinterpret_cast<To>(a_from);
        } else {
            union
            {
                std::remove_cv_t<std::remove_reference_t<From>> from;
                std::remove_cv_t<std::remove_reference_t<To>>   to;
            };

            from = std::forward<From>(a_from);
            return to;
        }
    }

    template <class Enum, class Underlying = std::underlying_type_t<Enum>>
    class enumeration
    {
    public:
        using enum_type       = Enum;
        using underlying_type = Underlying;

        static_assert(std::is_enum_v<enum_type>, "enum_type must be an enum");
        static_assert(std::is_integral_v<underlying_type>, "underlying_type must be an integral");

        constexpr enumeration() noexcept = default;

        constexpr enumeration(const enumeration&) noexcept = default;

        constexpr enumeration(enumeration&&) noexcept = default;

        template <class U2> // NOLINTNEXTLINE(google-explicit-constructor)
        constexpr enumeration(enumeration<Enum, U2> a_rhs) noexcept : _impl(static_cast<underlying_type>(a_rhs.get()))
        {
        }

        template <class... Args>
        constexpr enumeration(Args... a_values) noexcept
            requires(std::same_as<Args, enum_type> && ...)
            : _impl((static_cast<underlying_type>(a_values) | ...))
        {
        }

        ~enumeration() noexcept = default;

        constexpr enumeration& operator=(const enumeration&) noexcept = default;
        constexpr enumeration& operator=(enumeration&&) noexcept      = default;

        template <class U2>
        constexpr enumeration& operator=(enumeration<Enum, U2> a_rhs) noexcept
        {
            _impl = static_cast<underlying_type>(a_rhs.get());
            return *this;
        }

        constexpr enumeration& operator=(enum_type a_value) noexcept
        {
            _impl = static_cast<underlying_type>(a_value);
            return *this;
        }

        [[nodiscard]] explicit constexpr operator bool() const noexcept { return _impl != static_cast<underlying_type>(0); }

        [[nodiscard]] constexpr enum_type operator*() const noexcept { return get(); }

        [[nodiscard]] constexpr enum_type get() const noexcept { return static_cast<enum_type>(_impl); }

        [[nodiscard]] constexpr underlying_type underlying() const noexcept { return _impl; }

        template <class... Args>
        constexpr enumeration& set(Args... a_args) noexcept
            requires(std::same_as<Args, enum_type> && ...)
        {
            _impl |= (static_cast<underlying_type>(a_args) | ...);
            return *this;
        }

        template <class... Args>
        constexpr enumeration& reset(Args... a_args) noexcept
            requires(std::same_as<Args, enum_type> && ...)
        {
            _impl &= ~(static_cast<underlying_type>(a_args) | ...);
            return *this;
        }

        template <class... Args>
        [[nodiscard]] constexpr bool any(Args... a_args) const noexcept
            requires(std::same_as<Args, enum_type> && ...)
        {
            return (_impl & (static_cast<underlying_type>(a_args) | ...)) != static_cast<underlying_type>(0);
        }

        template <class... Args>
        [[nodiscard]] constexpr bool all(Args... a_args) const noexcept
            requires(std::same_as<Args, enum_type> && ...)
        {
            return (_impl & (static_cast<underlying_type>(a_args) | ...)) == (static_cast<underlying_type>(a_args) | ...);
        }

        template <class... Args>
        [[nodiscard]] constexpr bool none(Args... a_args) const noexcept
            requires(std::same_as<Args, enum_type> && ...)
        {
            return (_impl & (static_cast<underlying_type>(a_args) | ...)) == static_cast<underlying_type>(0);
        }

    private:
        underlying_type _impl{};
    };

    template <class... Args>
    enumeration(Args...) -> enumeration<std::common_type_t<Args...>>;

} // namespace stl
