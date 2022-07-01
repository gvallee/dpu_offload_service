# Endpoint cache

An endpoint cache is an array (dyn_array) of group caches. So if you have the
group_id, you know where it sits in the vector.
Each group cache as a vector of ranks so if you have the rank in the group, you know 
where it sits in that vector. In other words, each group gets its own rank cache.
And the final structure for that rank as a vector of service processes to define which SPs the rank is associated with.
