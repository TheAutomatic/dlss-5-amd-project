#pragma once
#include <cstddef>
#include <stdexcept>
#include <vector>

namespace FSRD
{
// CPU policy only. Payloads live in stable GPU allocations outside this class.
// Reserve happens before recording; Commit cannot allocate or wait for the GPU.
template <class Ticket> class RetiredSlotReservations
{
    std::vector<Ticket> tickets_;
    std::vector<size_t> reserved_;
    std::vector<size_t> selected_;
    size_t next_ = 0;

  public:
    bool HasReserved() const { return next_ < reserved_.size(); }
    size_t NextReserved() const
    {
        if (!HasReserved()) throw std::runtime_error("FSRD dispatch has no reserved slot");
        return reserved_[next_];
    }

    template <class Retired, class Grow>
    void Reserve(size_t count, Retired retired, Grow grow)
    {
        reserved_.clear();
        next_ = 0;
        selected_.clear();
        selected_.reserve(count);
        for (size_t i = 0; i < tickets_.size() && selected_.size() < count; ++i)
            if (tickets_[i] == Ticket{} || retired(tickets_[i]))
                selected_.push_back(i);

        if (selected_.size() < count)
        {
            const auto oldSize = tickets_.size();
            const auto size = grow(oldSize + count - selected_.size());
            if (size < oldSize + count - selected_.size())
                throw std::runtime_error("FSRD slot growth did not satisfy reservation");
            tickets_.resize(size);
            for (size_t i = oldSize; selected_.size() < count; ++i)
                selected_.push_back(i);
        }
        reserved_.swap(selected_);
    }

    size_t Commit(Ticket ticket)
    {
        if (!HasReserved() || ticket == Ticket{})
            throw std::runtime_error("FSRD dispatch has no reserved slot or valid recording ticket");
        const auto slot = reserved_[next_++];
        tickets_[slot] = ticket;
        return slot;
    }
};
} // namespace FSRD
