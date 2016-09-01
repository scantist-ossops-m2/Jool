#!/bin/bash

# Prepares the namespace environment where Jool will be enclosed.
# Run ./end.sh to undo these commands.

. config

ip netns add $NS

ip link add name $CLIENT_V6_INTERFACE type veth peer name $XLAT_V6_INTERFACE
ip link add name $CLIENT_V4_INTERFACE type veth peer name $XLAT_V4_INTERFACE
ip link set dev $XLAT_V6_INTERFACE netns $NS
ip link set dev $XLAT_V4_INTERFACE netns $NS

ip link set up dev $CLIENT_V6_INTERFACE
ip link set up dev $CLIENT_V4_INTERFACE
ip netns exec $NS ip link set up dev $XLAT_V6_INTERFACE
ip netns exec $NS ip link set up dev $XLAT_V4_INTERFACE
