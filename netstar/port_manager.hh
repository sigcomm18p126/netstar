#ifndef _PORT_MANAGER_HH
#define _PORT_MANAGER_HH

#include "core/distributed.hh"
#include "core/reactor.hh"

#include "net/patchfile/standard_device.hh"
#include "net/patchfile/fdir_device.hh"

#include "netstar/port.hh"
#include "netstar/shard_container.hh"

namespace netstar{

enum class port_type {
    standard,
    fdir
};

class stack_manager;

class port_manager {
    std::vector<port_type> _port_types;
    std::vector<std::unique_ptr<seastar::net::device>> _devs;
    std::vector<uint16_t> _port_ids;
    std::vector<std::vector<port*>> _ports;

    port_manager() {}

    using port_shard = shard_container_trait<port>;

public:
    static port_manager& get() {
        static port_manager pm;
        return pm;
    }

    seastar::future<> add_port(boost::program_options::variables_map& opts,
                               uint16_t port_id,
                               port_type pt){
        assert(port_check(opts, port_id));
        unsigned which_one = _ports.size();

        _port_types.push_back(pt);
        _port_ids.push_back(port_id);
        _ports.push_back(std::vector<port*>(seastar::smp::count, nullptr));

        switch(pt) {
        case(port_type::standard) : {
            auto dev = seastar::create_standard_device(port_id, seastar::smp::count);
            _devs.push_back(std::move(dev));
            break;
        }
        case(port_type::fdir) : {
            auto dev = seastar::create_fdir_device(port_id, seastar::smp::count);
            _devs.push_back(std::move(dev));
            break;
        }
        default : {
            break;
        }
        }

        auto dev  = _devs.back().get();
        auto port_shard_sptr = std::make_shared<port_shard::shard_t>();
        return port_shard_sptr->start(opts, dev, port_id).then([dev]{
            return dev->link_ready();
        }).then([this, which_one, port_shard_sptr]{
             return port_shard_sptr->invoke_on_all(&port_shard::instance_t::save_container_ptr,
                                                   &(_ports.at(which_one)));
        }).then([this, port_shard_sptr]{
            return port_shard_sptr->stop();
        }).then([port_shard_sptr]{
        });
    }

    port& pOrt(unsigned i) {
        return *(_ports.at(i).at(seastar::engine().cpu_id()));
    }

    port_type type(unsigned i) {
        return _port_types.at(i);
    }

    uint16_t dpdk_dev_idx(unsigned i) {
        return _port_ids.at(i);
    }

    unsigned num_ports() {
        return _ports.size();
    }

private:
    bool port_check(boost::program_options::variables_map& opts, uint16_t port_id){
        if(opts.count("network-stack") &&
           opts["network-stack"].as<std::string>() == "native"){
            seastar::fprint(std::cout, "port_manager ERROR: Can not use the device that the native stack is using.\n");
            return false;
        }

        for(auto id : _port_ids){
            if(id == port_id){
                seastar::fprint(std::cout, "port_manager ERROR: DPDK device with index %d has already been used.\n", port_id);
                return false;
            }
        }
        return true;
    }
    friend class stack_manager;
    seastar::net::device* dev(unsigned id) {
        return _devs.at(id).get();
    }
};

} // namespace netstar

#endif // _PORT_MANAGER_HH
